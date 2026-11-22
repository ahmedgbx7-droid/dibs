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

struct Extension {
    std::string name;
    std::string description;
    const char* (*exec)(const char* args_json) = nullptr;
    void* handle = nullptr;
};

static std::vector<Extension> loaded_extensions;

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

static void auto_compile_extensions(const fs::path& dir) {
    const char* compiler_env = std::getenv("DIBS_CXX");
    const std::string compiler = (compiler_env && *compiler_env) ? compiler_env : "clang++";

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) continue;
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") continue;

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
            std::cerr << "[Dibs] Extension is up to date: " << cpp_path.filename().string() << std::endl;
            continue;
        }

        fs::path temp_so = so_path;
        temp_so += ".tmp";

        std::cerr << "[Dibs] Compiling extension: " << cpp_path.filename().string() << "..." << std::endl;

        const std::string command = shell_quote(compiler) + " -std=c++17 -shared -fPIC -O2 " +
                                    shell_quote(cpp_path.string()) + " -o " + shell_quote(temp_so.string()) + " 2>&1";

        const int status = std::system(command.c_str());
        if (status == 0 && fs::exists(temp_so)) {
            fs::rename(temp_so, so_path, ec);
            if (ec) {
                ec.clear(); fs::remove(so_path, ec); ec.clear();
                fs::rename(temp_so, so_path, ec);
            }
            if (ec) {
                std::cerr << "[Dibs] Could not install compiled extension." << std::endl;
                fs::remove(temp_so, ec);
            } else {
                std::cerr << "[Dibs] Compiled: " << cpp_path.filename().string() << std::endl;
            }
        } else {
            std::cerr << "[Dibs] Compilation failed for " << cpp_path.filename().string() << std::endl;
            fs::remove(temp_so, ec);
        }
    }
}

static void load_extensions() {
    const char* custom_dir = std::getenv("DIBS_EXTENSIONS_DIR");
    const fs::path dir = (custom_dir && *custom_dir) ? fs::path(custom_dir) : fs::path("../extensions");

    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;

    auto_compile_extensions(dir);

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != ".so") continue;

        dlerror();
        void* handle = dlopen(entry.path().c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) continue;

        using name_fn_t = const char* (*)();
        using desc_fn_t = const char* (*)();
        using exec_fn_t = const char* (*)(const char*);

        auto name_fn = reinterpret_cast<name_fn_t>(dlsym(handle, "dibs_extension_name"));
        auto desc_fn = reinterpret_cast<desc_fn_t>(dlsym(handle, "dibs_extension_desc"));
        auto exec_fn = reinterpret_cast<exec_fn_t>(dlsym(handle, "dibs_extension_exec"));

        if (!name_fn || !desc_fn || !exec_fn) { dlclose(handle); continue; }

        try {
            std::string name(name_fn());
            std::string description(desc_fn());
            if (valid_tool_name(name)) {
                loaded_extensions.push_back({std::move(name), std::move(description), exec_fn, handle});
                std::cerr << "[Dibs] Loaded extension: " << loaded_extensions.back().name << std::endl;
            } else { dlclose(handle); }
        } catch (...) { dlclose(handle); }
    }
}

// ------------------ Built-in tools ------------------

static std::string exec_command(const std::string& cmd) {
    std::array<char, 512> buffer{};
    std::string result;
    const std::string full_cmd = "timeout 60s sh -c " + shell_quote(cmd) + " 2>&1";
    std::shared_ptr<FILE> pipe(popen(full_cmd.c_str(), "r"), pclose);
    if (!pipe) return "Error: Failed to run command.";
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get())) result += buffer.data();
    return result.empty() ? "Command executed (no output)." : result;
}

static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "Error: Could not open file.";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static std::string write_file(const std::string& path, const std::string& content) {
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
    std::string protocol = "http://", host = url;
    if (url.rfind("https://", 0) == 0) { protocol = "https://"; host = url.substr(8); }
    else if (url.rfind("http://", 0) == 0) { host = url.substr(7); }

    size_t slash_pos = host.find('/');
    std::string path = "/";
    if (slash_pos != std::string::npos) {
        path = host.substr(slash_pos);
        host = host.substr(0, slash_pos);
    }
    httplib::Client cli(protocol + host);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(10, 0);
    cli.enable_server_certificate_verification(false);
    auto res = cli.Get(path.c_str());
    if (res && res->status == 200) return res->body;
    return "Error: Failed to fetch URL.";
}

// ------------------ MCP helpers ------------------

static json text_content(const std::string& text) {
    return json::array({json{{"type", "text"}, {"text", text}}});
}

static bool parse_extension_mcp_result(const std::string& raw, json& content, bool& is_error) {
    try {
        const json parsed = json::parse(raw);
        if (!parsed.is_object() || !parsed.contains("_dibs_mcp_content") || !parsed["_dibs_mcp_content"].is_array())
            return false;

        json validated = json::array();
        for (const auto& item : parsed["_dibs_mcp_content"]) {
            if (!item.is_object() || !item.contains("type") || !item["type"].is_string()) continue;
            const std::string type = item["type"].get<std::string>();
            if (type == "text" && item.contains("text") && item["text"].is_string()) {
                validated.push_back({{"type", "text"}, {"text", item["text"]}});
            } else if (type == "image" && item.contains("data") && item["data"].is_string() &&
                       item.contains("mimeType") && item["mimeType"].is_string()) {
                if (item["mimeType"].get<std::string>().rfind("image/", 0) == 0)
                    validated.push_back({{"type", "image"}, {"data", item["data"]}, {"mimeType", item["mimeType"]}});
            }
        }
        if (validated.empty()) return false;
        content = std::move(validated);
        is_error = parsed.value("_dibs_is_error", false);
        return true;
    } catch (...) { return false; }
}

static json tool_definition(const std::string& name, const std::string& description) {
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", true}}}
    };
}

static json process_mcp_request(const json& req) {
    if (!req.is_object() || !req.contains("method") || !req["method"].is_string()) {
        return {{"jsonrpc", "2.0"}, {"id", req.value("id", json(nullptr))}, {"error", {{"code", -32600}, {"message", "Invalid Request"}}}};
    }

    const bool has_id = req.contains("id");
    const json id = has_id ? req["id"] : json(nullptr);
    const std::string method = req["method"].get<std::string>();

    if (!has_id) return json();

    json response = {{"jsonrpc", "2.0"}, {"id", id}};

    if (method == "initialize") {
        response["result"] = {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "dibs-mega"}, {"version", "1.2.0"}}}
        };
        return response;
    }

    if (method == "ping") {
        response["result"] = json::object();
        return response;
    }

    if (method == "tools/list") {
        json tools = json::array({
            {{"name", "run_command"}, {"description", "Execute a shell command with a 60s timeout."}, {"inputSchema", {{"type", "object"}, {"properties", {{"command", {{"type", "string"}}}}}, {"required", json::array({"command"})}}}},
            {{"name", "read_file"}, {"description", "Read a file."}, {"inputSchema", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}}}},
            {{"name", "write_file"}, {"description", "Write to a file."}, {"inputSchema", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}}, {"required", json::array({"path", "content"})}}}},
            {{"name", "list_dir"}, {"description", "List files in a directory."}, {"inputSchema", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}}}},
            {{"name", "fetch_url"}, {"description", "Fetch URL content over HTTP/HTTPS."}, {"inputSchema", {{"type", "object"}, {"properties", {{"url", {{"type", "string"}}}}}, {"required", json::array({"url"})}}}},
            {{"name", "get_battery"}, {"description", "Get battery percentage."}, {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
            {{"name", "sys_info"}, {"description", "Get system information."}, {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
            {{"name", "search_code"}, {"description", "Search for a text pattern recursively in files using grep."}, {"inputSchema", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}, {"pattern", {{"type", "string"}}}}}, {"required", json::array({"path", "pattern"})}}}},
            {{"name", "debug_binary"}, {"description", "Run a binary with gdb and return the backtrace if it crashes."}, {"inputSchema", {{"type", "object"}, {"properties", {{"binary_path", {{"type", "string"}}}, {"args", {{"type", "string"}}}}}, {"required", json::array({"binary_path"})}}}},
            {{"name", "get_thermal"}, {"description", "Get the CPU temperature of the device in Celsius."}, {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}}
        });

        for (const auto& ext : loaded_extensions) tools.push_back(tool_definition(ext.name, ext.description));
        response["result"] = {{"tools", tools}};
        return response;
    }

    if (method == "tools/call") {
        try {
            if (!req.contains("params") || !req["params"].is_object() || !req["params"].contains("name"))
                return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32602}, {"message", "Invalid params"}}}};

            const auto& params = req["params"];
            const std::string tool_name = params["name"].get<std::string>();
            const json args = params.value("arguments", json::object());

            std::string result_text; json content; bool is_error = false; bool extension_found = false;

            if (tool_name == "run_command") result_text = exec_command(args["command"]);
            else if (tool_name == "read_file") result_text = read_file(args["path"]);
            else if (tool_name == "write_file") result_text = write_file(args["path"], args["content"]);
            else if (tool_name == "list_dir") result_text = list_dir(args["path"]);
            else if (tool_name == "fetch_url") result_text = fetch_url(args["url"]);
            else if (tool_name == "get_battery") {
                result_text = read_file("/sys/class/power_supply/battery/capacity");
                if (result_text.rfind("Error:", 0) == 0) is_error = true;
                else result_text = "Battery: " + result_text + "%";
            }
            else if (tool_name == "sys_info") result_text = exec_command("uname -a && df -h /data && free -h");
            else if (tool_name == "search_code") result_text = exec_command("grep -rn " + shell_quote(args["pattern"].get<std::string>()) + " " + shell_quote(args["path"].get<std::string>()));
            else if (tool_name == "debug_binary") result_text = exec_command("gdb -batch -ex run -ex bt --args " + shell_quote(args["binary_path"].get<std::string>()) + " " + args.value("args", ""));
            else if (tool_name == "get_thermal") {
                std::string raw_temp = read_file("/sys/class/thermal/thermal_zone0/temp");
                if (raw_temp.rfind("Error:", 0) == 0) { result_text = raw_temp; is_error = true; }
                else {
                    try { result_text = "CPU Temperature: " + std::to_string(std::stod(raw_temp) / 1000.0).substr(0, 4) + " °C"; }
                    catch (...) { result_text = "Error parsing temperature"; is_error = true; }
                }
            }
            else {
                for (const auto& ext : loaded_extensions) {
                    if (tool_name == ext.name) {
                        extension_found = true;
                        const char* ext_result = ext.exec(args.dump().c_str());
                        if (!ext_result) { result_text = "Extension returned null."; is_error = true; }
                        else {
                            result_text = ext_result;
                            if (!parse_extension_mcp_result(result_text, content, is_error)) content = text_content(result_text);
                        }
                        break;
                    }
                }
                if (!extension_found) return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "Tool not found: " + tool_name}}}};
            }

            if (content.is_null()) content = text_content(result_text);
            response["result"] = {{"content", content}};
            if (is_error) response["result"]["isError"] = true;
            return response;

        } catch (const std::exception& e) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", text_content(std::string("Error: ") + e.what())}, {"isError", true}}}};
        }
    }
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "Method not found"}}}};
}

int main() {
    load_extensions();

    httplib::Server svr_http;

    auto setup_routes = [](auto& server) {
        server.Post("/mcp", [](const httplib::Request& req, httplib::Response& res) {
            try {
                const json request = json::parse(req.body);
                const json response = process_mcp_request(request);
                if (response.is_null()) { res.status = 202; res.set_content("", "application/json"); return; }
                res.set_content(response.dump(), "application/json");
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
                res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
            } catch (const json::exception&) {
                res.status = 400; res.set_content("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}", "application/json");
            } catch (const std::exception& e) {
                res.status = 500; res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        server.Options("/mcp", [](const httplib::Request&, httplib::Response& res) {
            res.status = 204;
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
            res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        });
    };

    setup_routes(svr_http);

    std::cout << "========================================\n";
    std::cout << "Dibs Mega MCP Server v1.2.0 is running!\n";
    std::cout << "HTTP Endpoint:  http://127.0.0.1:8080/mcp\n";
    std::cout << "========================================\n";

    if (!svr_http.listen("0.0.0.0", 8080)) {
        std::cerr << "[Dibs] Failed to bind HTTP server." << std::endl;
        return 1;
    }
    return 0;
}
