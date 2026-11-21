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

using json = nlohmann::json;
namespace fs = std::filesystem;

struct Extension {
    std::string name;
    std::string description;
    const char* (*exec)(const char* args_json);
};
std::vector<Extension> loaded_extensions;

// دالة بتترجم كود C++ لملف .so أوتوماتيك
void auto_compile_extensions(const std::string& dir) {
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".cpp") {
            std::string cpp_path = entry.path().string();
            std::string so_path = cpp_path;
            size_t ext_pos = so_path.find_last_of(".");
            so_path = so_path.substr(0, ext_pos) + ".so";
            
            std::cerr << "[Dibs] Auto-compiling: " << entry.path().filename().string() << "..." << std::endl;
            
            std::string cmd = "clang++ -shared -fPIC " + cpp_path + " -o " + so_path + " 2>&1";
            int result = system(cmd.c_str());
            
            if (result != 0) {
                std::cerr << "[Dibs] Warning: Failed to compile " << entry.path().filename().string() << std::endl;
            }
        }
    }
}

// دالة تحميل الإضافات
void load_extensions() {
    std::string dir = "../extensions";
    if (!fs::exists(dir)) return;

    auto_compile_extensions(dir);

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".so") continue;
        void* handle = dlopen(entry.path().c_str(), RTLD_NOW);
        if (!handle) {
            std::cerr << "[Dibs] Failed to load: " << entry.path().filename().string() << " (" << dlerror() << ")" << std::endl;
            continue;
        }

        auto name_fn = (const char*(*)())dlsym(handle, "dibs_extension_name");
        auto desc_fn = (const char*(*)())dlsym(handle, "dibs_extension_desc");
        auto exec_fn = (const char*(*)(const char*))dlsym(handle, "dibs_extension_exec");

        if (name_fn && desc_fn && exec_fn) {
            loaded_extensions.push_back({name_fn(), desc_fn(), exec_fn});
            std::cerr << "[Dibs] Loaded extension: " << name_fn() << std::endl;
        }
    }
}

// ------------------ الأدوات الأساسية ------------------
std::string exec_command(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    // هنا التعديل: استخدمنا bash -c عشان يقدر ينفذ أوامر زي cd
    std::string full_cmd = "timeout 15s bash -c \"" + cmd + "\" 2>&1";
    std::shared_ptr<FILE> pipe(popen(full_cmd.c_str(), "r"), pclose);
    if (!pipe) return "Error: Failed to run command.";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    return result.empty() ? "Command executed successfully (no output)." : result;
}
std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "Error: Could not open file.";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
std::string write_file(const std::string& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) return "Error: Could not write to file.";
    file << content;
    return "File written successfully.";
}
std::string list_dir(const std::string& path) {
    std::string result;
    try {
        for (const auto& entry : fs::directory_iterator(path)) result += entry.path().filename().string() + "\n";
        return result.empty() ? "Directory is empty." : result;
    } catch (...) { return "Error: Could not read directory."; }
}
std::string fetch_url(const std::string& url) {
    std::string protocol = "http://", host = url;
    if (url.find("https://") == 0) { protocol = "https://"; host = url.substr(8); }
    else if (url.find("http://") == 0) { host = url.substr(7); }
    size_t slash_pos = host.find('/');
    std::string path = "/";
    if (slash_pos != std::string::npos) { path = host.substr(slash_pos); host = host.substr(0, slash_pos); }
    httplib::Client cli(protocol + host);
    auto res = cli.Get(path.c_str());
    return (res && res->status == 200) ? res->body : "Error: Failed to fetch URL.";
}

// ------------------ سيرفر MCP ------------------
json process_mcp_request(const json& req) {
    if (!req.contains("id")) return json();
    json res = {{"jsonrpc", "2.0"}, {"id", req["id"]}};
    std::string method = req["method"];

    if (method == "initialize") {
        res["result"] = {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", {{"tools", json::object()}}},
            {"serverInfo", {{"name", "dibs-mega"}, {"version", "1.0.0"}}}
        };
    } 
    else if (method == "tools/list") {
        json tools = json::array({
            {{"name", "run_command"}, {"description", "Execute a shell command (Termux/Linux) with a 15s timeout."}, {"inputSchema", {{"type", "object"}, {"properties", {{"command", {{"type", "string"}}}}}, {"required", json::array({"command"})}}}},
            {{"name", "read_file"}, {"description", "Read a file."}, {"inputSchema", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}}}},
            {{"name", "write_file"}, {"description", "Write to a file."}, {"inputSchema", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}}, {"required", json::array({"path", "content"})}}}},
            {{"name", "list_dir"}, {"description", "List files in a directory."}, {"inputSchema", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}}}},
            {{"name", "fetch_url"}, {"description", "Fetch URL content."}, {"inputSchema", {{"type", "object"}, {"properties", {{"url", {{"type", "string"}}}}}, {"required", json::array({"url"})}}}},
            {{"name", "get_battery"}, {"description", "Get battery percentage."}, {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
            {{"name", "sys_info"}, {"description", "Get system information."}, {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}}
        });

        for (auto& ext : loaded_extensions) {
            tools.push_back({
                {"name", ext.name},
                {"description", ext.description},
                {"inputSchema", {{"type", "object"}, {"properties", {{"args", {{"type", "string"}}}}}}}
            });
        }
        res["result"]["tools"] = tools;
    } 
    else if (method == "tools/call") {
        auto params = req["params"];
        std::string tool_name = params["name"];
        auto args = params["arguments"];
        std::string result_text = "";

        try {
            if (tool_name == "run_command") result_text = exec_command(args["command"]);
            else if (tool_name == "read_file") result_text = read_file(args["path"]);
            else if (tool_name == "write_file") result_text = write_file(args["path"], args["content"]);
            else if (tool_name == "list_dir") result_text = list_dir(args["path"]);
            else if (tool_name == "fetch_url") result_text = fetch_url(args["url"]);
            else if (tool_name == "get_battery") result_text = "Battery: " + read_file("/sys/class/power_supply/battery/capacity") + "%";
            else if (tool_name == "sys_info") result_text = exec_command("uname -a && df -h /data && free -h");
            else {
                bool found = false;
                for (auto& ext : loaded_extensions) {
                    if (tool_name == ext.name) {
                        const char* ext_res = ext.exec(args.dump().c_str());
                        result_text = ext_res ? ext_res : "Extension returned null.";
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    res["error"] = {{"code", -32601}, {"message", "Tool not found"}};
                    return res;
                }
            }
            res["result"] = {{"content", json::array({{{"type", "text"}, {"text", result_text}}})}};
        } catch (const std::exception& e) {
            res["result"] = {{"content", json::array({{{"type", "text"}, {"text", "Error: " + std::string(e.what())}}})}, {"isError", true}};
        }
    } 
    else res["error"] = {{"code", -32601}, {"message", "Method not found"}};
    
    return res;
}

int main() {
    load_extensions();
    httplib::Server svr;
    svr.Post("/mcp", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto req_json = json::parse(req.body);
            res.set_content(process_mcp_request(req_json).dump(), "application/json");
            res.set_header("Access-Control-Allow-Origin", "*");
        } catch (...) { res.status = 400; res.set_content("{\"error\": \"Invalid JSON\"}", "application/json"); }
    });

    std::cout << "========================================\n";
    std::cout << "Dibs Mega MCP Server is running!\n";
    std::cout << "Your Link: http://127.0.0.1:8080/mcp\n";
    std::cout << "========================================\n";
    svr.listen("0.0.0.0", 8080);
    return 0;
}
