#include <string>
#include <nlohmann/json.hpp>
#include <iostream>
#include <cstdlib>

extern "C" const char* dibs_extension_name() {
    return "speak";
}

extern "C" const char* dibs_extension_desc() {
    return "Speaks text out loud on the device using Termux text-to-speech. "
           "Use this tool when the user asks you to say something aloud, read text aloud, or give a spoken notification. "
           "Required input: a JSON object with a single string field \"text\" containing the words to speak.";
}

extern "C" const char* dibs_extension_exec(const char* args_json) {
    static std::string response;

    try {
        nlohmann::json args = nlohmann::json::parse(args_json ? args_json : "");

        if (!args.contains("text") || !args["text"].is_string()) {
            response = "Error: missing required string argument \"text\"";
            return response.c_str();
        }

        std::string text = args["text"].get<std::string>();

        // Escape for the shell: wrap in single quotes and escape any single quote inside.
        // This prevents shell injection through $(...), backticks, or quotes in the text.
        std::string escaped;
        for (char c : text) {
            if (c == '\'') {
                escaped += "'\\''";
            } else {
                escaped += c;
            }
        }

        std::string cmd = "termux-tts-speak '" + escaped + "'";
        int rc = system(cmd.c_str());

        if (rc != 0) {
            response = "Error: termux-tts-speak failed (exit code " + std::to_string(rc) + "). "
                       "Make sure Termux:API is installed.";
        } else {
            response = "Speaking: " + text;
        }
    } catch (const std::exception& e) {
        response = std::string("Error: invalid arguments - ") + e.what();
    }

    return response.c_str();
}
