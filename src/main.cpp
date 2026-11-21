#include <iostream>
#include <string>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <cstdio>
#include <memory>
#include <array>
#include <dlfcn.h>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <system_error>

using json = nlohmann::json;
namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// Dibs extension ABI
//
// Required extension exports:
//   extern "C" const char* dibs_extension_name();
//   extern "C" const char* dibs_extension_desc();
//   extern "C" const char* dibs_extension_exec(const char* args_json);
//
// Optional image-capable return format from dibs_extension_exec():
// {
//   "_dibs_mcp_content": [
//     {"type":"text", "text":"Screenshot captured"},
//     {"type":"image", "data":"<BASE64>", "mimeType":"image/png"}
//   ],
//   "_dibs_is_error": false
// }
//
// If the extension returns ordinary text or ordinary JSON, Dibs returns it
// as a normal MCP text content item. Image bytes must be base64 encoded.
// -----------------------------------------------------------------------------

struct Extension {
    std::string name;
    std::string description;
    const char* (*exec)(const char* args_json) = nullptr;
    void* handle = nullptr; // Keep the shared library loaded for function pointers.
};

static std::vector<Extension> loaded_extensions;

// Quote one shell argument safely using single quotes.
static std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static bool valid_tool_name(const std::string& name) {
    if (name.empty()) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::islower(c) || std::isdigit(c) || c == '_' || c == '-';
    });
}

// Compile extensions only when their .cpp file is newer than the .so file.
static void auto_compile_extensions(const fs::path& dir) {
    const char* compiler_env = std::getenv("DIBS_CXX");
    const std::string compiler =
        (compiler_env && *compiler_env) ? compiler_env : "clang++";

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            std::cerr << "[Dibs] Cannot scan extensions directory: "
                      << ec.message() << std::endl;
            return;
        }

        if (!entry.is_regular_file() || entry.path().extension() != ".cpp")
            continue;

        const fs::path cpp_path = entry.path();
        fs::path so_path = cpp_path;
        so_path.replace_extension(".so");

        bool needs_build = !fs::exists(so_path);
        if (!needs_build) {
            const auto cpp_time = fs::last_write_time(cpp_path, ec);
            if (ec) { ec.clear(); needs_build = true; }
            else {
                const auto so_time = fs::last_write_time(so_path, ec);
                if (ec) { ec.clear(); needs_build = true; }
                else needs_build = cpp_time > so_time;
            }
        }

        if (!needs_build) {
            std::cerr << "[Dibs] Extension is up to date: "
                      << cpp_path.filename().string() << std::endl;
            continue;
        }

        fs::path temp_so = so_path;
        temp_so += ".tmp";

        std::cerr << "[Dibs] Compiling extension: "
                  << cpp_path.filename().string() << "..." << std::endl;

        // The temporary output avoids replacing a working .so with a broken build.
        const std::string command =
            shell_quote(compiler) +
            " -std=c++17 -shared -fPIC -O2 " +
            shell_quote(cpp_path.string()) +
            " -o " + shell_quote(temp_so.string()) +
            " 2>&1";

        const int status = std::system(command.c_str());
        if (status == 0 && fs::exists(temp_so)) {
            fs::rename(temp_so, so_path, ec);
            if (ec) {
                // Android/filesystems may not replace an existing file atomically.
                ec.clear();
                fs::remove(so_path, ec);
                ec.clear();
                fs::rename(temp_so, so_path, ec);
            }
            if (ec) {
                std::cerr << "[Dibs] Could not install compiled extension: "
                          << ec.message() << std::endl;
                fs::remove(temp_so, ec);
            } else {
                std::cerr << "[Dibs] Compiled: "
                          << cpp_path.filename().string() << std::endl;
            }
        } else {
            std::cerr << "[Dibs] Compilation failed for "
                      << cpp_path.filename().string()
                      << ". Check compiler output above." << std::endl;
            fs::remove(temp_so, ec);
        }
    }
}

// Load extension shared libraries and retain their handles for process lifetime.
static void load_extensions() {
    const char* custom_dir = std::getenv("DIBS_EXTENSIONS_DIR");
    const fs::path dir =
        (custom_dir && *custom_dir) ? fs::path(custom_dir) : fs::path("../extensions");

    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        std::cerr << "[Dibs] Extensions directory not found: "
                  << dir.string() << std::endl;
        return;
    }

    auto_compile_extensions(dir);

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != ".so")
            continue;

        dlerror();
        void* handle = dlopen(entry.path().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            const char* err = dlerror();
            std::cerr << "[Dibs] Failed to load "
                      << entry.path().filename().string() << ": "
                      << (err ? err : "unknown error") << std::endl;
            continue;
        }

        using name_fn_t = const char* (*)();
        using desc_fn_t = const char* (*)();
        using exec_fn_t = const char* (*)(const char*);

        auto name_fn = reinterpret_cast<name_fn_t>(
            dlsym(handle, "dibs_extension_name"));
        auto desc_fn = reinterpret_cast<desc_fn_t>(
            dlsym(handle, "dibs_extension_desc"));
        auto exec_fn = reinterpret_cast<exec_fn_t>(
            dlsym(handle, "dibs_extension_exec"));

        if (!name_fn || !desc_fn || !exec_fn) {
            std::cerr << "[Dibs] Missing required export in "
                      << entry.path().filename().string() << std::endl;
            dlclose(handle);
            continue;
        }

        try {
            const char* raw_name = name_fn();
            const char* raw_desc = desc_fn();
            if (!raw_name || !raw_desc) {
                std::cerr << "[Dibs] Null name/description in "
                          << entry.path().filename().string() << std::endl;
                dlclose(handle);
                continue;
            }

            std::string name(raw_name);
            std::string description(raw_desc);

            if (!valid_tool_name(name)) {
                std::cerr << "[Dibs] Invalid extension tool name: "
                          << name << std::endl;
                dlclose(handle);
                continue;
            }

            const bool duplicate = std::any_of(
                loaded_extensions.begin(), loaded_extensions.end(),
                [&](const Extension& ext) { return ext.name == name; });

            if (duplicate) {
                std::cerr << "[Dibs] Duplicate extension tool name: "
                          << name << std::endl;
                dlclose(handle);
                continue;
            }

            loaded_extensions.push_back(
                {std::move(name), std::move(description), exec_fn, handle});

            std::cerr << "[Dibs] Loaded extension: "
                      << loaded_extensions.back().name << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[Dibs] Extension initialization error: "
                      << e.what() << std::endl;
            dlclose(handle);
        }
    }
}

// -----------------------------------------------------------------------------
// Built-in tools
// -----------------------------------------------------------------------------

static std::string exec_command(const std::string& cmd) {
    std::array<char, 512> buffer{};
    std::string result;

    // Use sh -c so shell built-ins such as cd work within the command string.
    // timeout applies to the shell process; it cannot time-limit a shell built-in
    // independently of that shell.
    const std::string full_cmd =
        "timeout 15s sh -c " + shell_quote(cmd) + " 2>&1";

    std::shared_ptr<FILE> pipe(popen(full_cmd.c_str(), "r"), pclose);
    if (!pipe) return "Error: Failed to run command.";

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()))
        result += buffer.data();

    return result.empty()
        ? "Command executed (no output; exit status not reported)."
        : result;
}

static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "Error: Could not open file.";
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

static std::string write_file(const std::string& path,
                              const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return "Error: Could not write to file.";
    file << content;
    if (!file.good()) return "Error: Failed while writing file.";
    return "File written successfully.";
}

static std::string list_dir(const std::string& path) {
    std::string result;
    try {
        for (const auto& entry : fs::directory_iterator(path))
            result += entry.path().filename().string() + "\n";
        return result.empty() ? "Directory is empty." : result;
    } catch (const std::exception& e) {
        return "Error: Could not read directory: " + std::string(e.what());
    }
}

static std::string fetch_url(const std::string& url) {
    // cpp-httplib's HTTPS support depends on how that library was built.
    // This preserves the original HTTP behavior and avoids pretending HTTPS
    // worked when the SSL-enabled client is unavailable.
    if (url.rfind("https://", 0) == 0)
        return "Error: HTTPS requires an SSL-enabled cpp-httplib build.";

    std::string host = url;
    if (host.rfind("http://", 0) == 0) host = host.substr(7);

    const size_t slash_pos = host.find('/');
    std::string path = "/";
    if (slash_pos != std::string::npos) {
        path = host.substr(slash_pos);
        host = host.substr(0, slash_pos);
    }

    httplib::Client cli("http://" + host);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(10, 0);
    auto res = cli.Get(path.c_str());

    if (res && res->status == 200) return res->body;
    return "Error: Failed to fetch URL.";
}

// -----------------------------------------------------------------------------
// MCP helpers
// -----------------------------------------------------------------------------

static json text_content(const std::string& text) {
    return json::array({json{{"type", "text"}, {"text", text}}});
}

// An extension can return a JSON envelope to send MCP text/image content.
// Invalid envelopes safely fall back to a plain text result.
static bool parse_extension_mcp_result(const std::string& raw,
                                       json& content,
                                       bool& is_error) {
    try {
        const json parsed = json::parse(raw);
        if (!parsed.is_object() ||
            !parsed.contains("_dibs_mcp_content") ||
            !parsed["_dibs_mcp_content"].is_array()) {
            return false;
        }

        json validated = json::array();
        for (const auto& item : parsed["_dibs_mcp_content"]) {
            if (!item.is_object() || !item.contains("type") ||
                !item["type"].is_string()) {
                continue;
            }

            const std::string type = item["type"].get<std::string>();

            if (type == "text" &&
                item.contains("text") && item["text"].is_string()) {
                validated.push_back({
                    {"type", "text"},
                    {"text", item["text"]}
                });
            } else if (type == "image" &&
                       item.contains("data") && item["data"].is_string() &&
                       item.contains("mimeType") && item["mimeType"].is_string()) {
                const std::string mime = item["mimeType"].get<std::string>();
                // Restrict image content to common image MIME types.
                if (mime.rfind("image/", 0) == 0) {
                    validated.push_back({
                        {"type", "image"},
                        {"data", item["data"]},
                        {"mimeType", mime}
                    });
                }
            }
        }

        if (validated.empty()) return false;

        content = std::move(validated);
        is_error = parsed.value("_dibs_is_error", false);
        return true;
    } catch (...) {
        return false;
    }
}

static json tool_definition(const std::string& name,
                            const std::string& description) {
    // Extension-specific parameters are documented in the description.
    // additionalProperties=true lets extensions receive ordinary JSON objects
    // directly, instead of forcing every tool into {"args":"..."}.
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()},
            {"additionalProperties", true}
        }}
    };
}

static json process_mcp_request(const json& req) {
    if (!req.is_object() || !req.contains("method") ||
        !req["method"].is_string()) {
        json invalid = {
            {"jsonrpc", "2.0"},
            {"error", {{"code", -32600}, {"message", "Invalid Request"}}}
        };
        if (req.is_object() && req.contains("id")) invalid["id"] = req["id"];
        else invalid["id"] = nullptr;
        return invalid;
    }

    // JSON-RPC notifications do not receive a response.
    const bool has_id = req.contains("id");
    const json id = has_id ? req["id"] : json(nullptr);
    const std::string method = req["method"].get<std::string>();

    if (!has_id) {
        if (method == "notifications/initialized") return json();
        return json();
    }

    json response = {{"jsonrpc", "2.0"}, {"id", id}};

    if (method == "initialize") {
        response["result"] = {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {
                {"name", "dibs-mega"},
                {"version", "1.1.0"}
            }}
        };
        return response;
    }

    if (method == "ping") {
        response["result"] = json::object();
        return response;
    }

    if (method == "tools/list") {
        json tools = json::array({
            {
                {"name", "run_command"},
                {"description", "Execute a shell command in Termux/Linux. Commands run through sh with a 15-second timeout. Use cautiously."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"command", {{"type", "string"},
                                     {"description", "Shell command to execute"}}}
                    }},
                    {"required", json::array({"command"})}
                }}
            },
            {
                {"name", "read_file"},
                {"description", "Read a UTF-8 or binary-compatible text file from an accessible path."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}}}
                    }},
                    {"required", json::array({"path"})}
                }}
            },
            {
                {"name", "write_file"},
                {"description", "Write text content to a file. Overwrites the destination file."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}}},
                        {"content", {{"type", "string"}}}
                    }},
                    {"required", json::array({"path", "content"})}
                }}
            },
            {
                {"name", "list_dir"},
                {"description", "List the entries in a directory."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}}}
                    }},
                    {"required", json::array({"path"})}
                }}
            },
            {
                {"name", "fetch_url"},
                {"description", "Fetch a URL over HTTP. HTTPS needs an SSL-enabled cpp-httplib build."},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"url", {{"type", "string"}}}
                    }},
                    {"required", json::array({"url"})}
                }}
            },
            {
                {"name", "get_battery"},
                {"description", "Read the Android/Linux battery percentage when the sysfs path is available."},
                {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
            },
            {
                {"name", "sys_info"},
                {"description", "Get basic operating-system and storage information."},
                {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}
            }
        });

        for (const auto& ext : loaded_extensions)
            tools.push_back(tool_definition(ext.name, ext.description));

        response["result"] = {{"tools", tools}};
        return response;
    }

    if (method == "tools/call") {
        try {
            if (!req.contains("params") || !req["params"].is_object() ||
                !req["params"].contains("name") ||
                !req["params"]["name"].is_string()) {
                response["error"] = {
                    {"code", -32602},
                    {"message", "Invalid params: expected params.name"}
                };
                return response;
            }

            const auto& params = req["params"];
            const std::string tool_name = params["name"].get<std::string>();
            const json args = params.value("arguments", json::object());

            if (!args.is_object()) {
                response["error"] = {
                    {"code", -32602},
                    {"message", "Tool arguments must be a JSON object"}
                };
                return response;
            }

            std::string result_text;
            json content;
            bool is_error = false;
            bool extension_found = false;

            if (tool_name == "run_command") {
                if (!args.contains("command") || !args["command"].is_string())
                    throw std::runtime_error("Missing string argument: command");
                result_text = exec_command(args["command"].get<std::string>());
            } else if (tool_name == "read_file") {
                if (!args.contains("path") || !args["path"].is_string())
                    throw std::runtime_error("Missing string argument: path");
                result_text = read_file(args["path"].get<std::string>());
            } else if (tool_name == "write_file") {
                if (!args.contains("path") || !args["path"].is_string() ||
                    !args.contains("content") || !args["content"].is_string())
                    throw std::runtime_error("Expected string arguments: path, content");
                result_text = write_file(args["path"].get<std::string>(),
                                          args["content"].get<std::string>());
            } else if (tool_name == "list_dir") {
                if (!args.contains("path") || !args["path"].is_string())
                    throw std::runtime_error("Missing string argument: path");
                result_text = list_dir(args["path"].get<std::string>());
            } else if (tool_name == "fetch_url") {
                if (!args.contains("url") || !args["url"].is_string())
                    throw std::runtime_error("Missing string argument: url");
                result_text = fetch_url(args["url"].get<std::string>());
            } else if (tool_name == "get_battery") {
                result_text = read_file("/sys/class/power_supply/battery/capacity");
                if (result_text.rfind("Error:", 0) == 0) {
                    is_error = true;
                } else {
                    while (!result_text.empty() &&
                           (result_text.back() == '\n' || result_text.back() == '\r'))
                        result_text.pop_back();
                    result_text = "Battery: " + result_text + "%";
                }
            } else if (tool_name == "sys_info") {
                result_text = exec_command("uname -a && df -h /data && free -h");
            } else {
                for (const auto& ext : loaded_extensions) {
                    if (tool_name != ext.name) continue;

                    extension_found = true;
                    const std::string args_json = args.dump();
                    const char* ext_result = ext.exec(args_json.c_str());

                    if (!ext_result) {
                        result_text = "Extension returned null.";
                        is_error = true;
                    } else {
                        result_text = ext_result;
                        if (!parse_extension_mcp_result(
                                result_text, content, is_error)) {
                            content = text_content(result_text);
                        }
                    }
                    break;
                }

                if (!extension_found) {
                    response["error"] = {
                        {"code", -32601},
                        {"message", "Tool not found: " + tool_name}
                    };
                    return response;
                }
            }

            if (content.is_null()) content = text_content(result_text);

            response["result"] = {{"content", content}};
            if (is_error) response["result"]["isError"] = true;
            return response;

        } catch (const std::exception& e) {
            response["result"] = {
                {"content", text_content(std::string("Error: ") + e.what())},
                {"isError", true}
            };
            return response;
        }
    }

    response["error"] = {
        {"code", -32601},
        {"message", "Method not found: " + method}
    };
    return response;
}

// -----------------------------------------------------------------------------
// HTTP server
// -----------------------------------------------------------------------------

int main() {
    load_extensions();

    httplib::Server server;

    server.Post("/mcp", [](const httplib::Request& req,
                           httplib::Response& res) {
        try {
            const json request = json::parse(req.body);
            const json response = process_mcp_request(request);

            // Notifications have no JSON-RPC response body.
            if (response.is_null()) {
                res.status = 202;
                res.set_content("", "application/json");
                return;
            }

            res.set_content(response.dump(), "application/json");
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Headers",
                           "Content-Type, Authorization");
            res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        } catch (const json::exception& e) {
            res.status = 400;
            res.set_content(
                json{{"jsonrpc", "2.0"},
                     {"error", {{"code", -32700},
                                {"message", std::string("Parse error: ") + e.what()}}}}
                    .dump(),
                "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(
                json{{"error", std::string("Internal server error: ") + e.what()}}
                    .dump(),
                "application/json");
        }
    });

    server.Options("/mcp", [](const httplib::Request&,
                              httplib::Response& res) {
        res.status = 204;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Headers",
                       "Content-Type, Authorization");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
    });

    // Bind to loopback by default: the companion app and Dibs run on this phone.
    // Set DIBS_HOST=0.0.0.0 only if you intentionally need LAN access.
    const char* host_env = std::getenv("DIBS_HOST");
    const std::string host =
        (host_env && *host_env) ? host_env : "127.0.0.1";

    const char* port_env = std::getenv("DIBS_PORT");
    int port = 8080;
    if (port_env && *port_env) {
        try {
            const int parsed = std::stoi(port_env);
            if (parsed > 0 && parsed <= 65535) port = parsed;
        } catch (...) {
            std::cerr << "[Dibs] Invalid DIBS_PORT; using 8080." << std::endl;
        }
    }

    std::cout << "========================================\n";
    std::cout << "Dibs MCP Server v1.1.0 is running!\n";
    std::cout << "Endpoint: http://" << host << ":" << port << "/mcp\n";
    std::cout << "Loaded extensions: " << loaded_extensions.size() << "\n";
    std::cout << "========================================\n";

    if (!server.listen(host.c_str(), port)) {
        std::cerr << "[Dibs] Failed to bind HTTP server at "
                  << host << ":" << port << std::endl;
        return 1;
    }

    return 0;
}
