#include <string>
#include <ctime>

extern "C" const char* dibs_extension_name() {
    return "get_time";
}

extern "C" const char* dibs_extension_desc() {
    return "Get the current date and time of the system. No arguments required.";
}

extern "C" const char* dibs_extension_exec(const char* args_json) {
    static std::string response;
    std::time_t now = std::time(nullptr);
    std::string time_str = std::ctime(&now);
    
    // تنظيف النص من سطر جديد في الآخر
    if (!time_str.empty() && time_str.back() == '\n') {
        time_str.pop_back();
    }
    
    response = "Current system time: " + time_str;
    return response.c_str();
}
