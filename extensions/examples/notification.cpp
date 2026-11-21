#include <string>
#include <nlohmann/json.hpp>
#include <cstdlib>

extern "C" const char* dibs_extension_name() { return "send_notification"; }
extern "C" const char* dibs_extension_desc() { return "Send an Android notification via Termux API. Args: {title, message}"; }

extern "C" const char* dibs_extension_exec(const char* args_json) {
    static std::string response;
    try {
        auto args = nlohmann::json::parse(args_json);
        std::string title = args.value("title", "Dibs AI");
        std::string msg = args.value("message", "Hello from Dibs!");
        std::string cmd = "termux-notification --title "" + title + "" --content "" + msg + """;
        system(cmd.c_str());
        response = "Notification sent successfully.";
    } catch (...) {
        response = "Error: Invalid arguments. Use {title, message}.";
    }
    return response.c_str();
}