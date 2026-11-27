#include <string>
#include <cstdlib>
#include <nlohmann/json.hpp>

// Example Dibs extension: sends an Android notification through Termux:API.
// Build manually:  clang++ -std=c++17 -shared -fPIC -O2 notification.cpp -o notification.so
// Or copy it into ../ (the extensions/ folder) and Dibs compiles it on startup.

static std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

extern "C" const char* dibs_extension_name() { return "send_notification"; }

extern "C" const char* dibs_extension_desc() {
    return "Send an Android notification via Termux:API. "
           "Args: {\"title\": string (optional), \"message\": string (optional)}";
}

extern "C" const char* dibs_extension_exec(const char* args_json) {
    static std::string response;
    try {
        auto args = nlohmann::json::parse(args_json ? args_json : "{}");
        std::string title = args.value("title", "Dibs AI");
        std::string msg   = args.value("message", "Hello from Dibs!");

        // Quote every user-supplied value to prevent shell injection.
        std::string cmd = "termux-notification --title " + shell_quote(title) +
                          " --content " + shell_quote(msg);
        int rc = std::system(cmd.c_str());
        response = (rc == 0) ? "Notification sent successfully."
                             : "Error: termux-notification failed. Is Termux:API installed?";
    } catch (const std::exception& e) {
        response = std::string("Error: invalid arguments - ") + e.what();
    }
    return response.c_str();
}
