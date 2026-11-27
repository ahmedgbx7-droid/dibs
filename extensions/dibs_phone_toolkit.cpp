#include <nlohmann/json.hpp>
#include <string>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;
static std::string response;

static std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string run_cmd(const std::string& cmd, int* status = nullptr) {
    std::array<char, 4096> buf{};
    std::string out;
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) {
        if (status) *status = -1;
        return "Unable to start command";
    }
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
        if (out.size() > 1024 * 1024) {
            out.resize(1024 * 1024);
            break;
        }
    }
    int rc = pclose(pipe);
    if (status) *status = rc;
    return out;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string base64_encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(t[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(t[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

static json error_json(const std::string& msg) {
    return json{{"error", msg}, {"isError", true}};
}

extern "C" const char* dibs_extension_name() {
    return "phone_toolkit";
}

extern "C" const char* dibs_extension_desc() {
    return "Phone toolkit: device info, clipboard, notifications, camera, location and sensors via Termux:API.";
}

extern "C" const char* dibs_extension_exec(const char* args_json) {
    try {
        json a = json::parse(args_json ? args_json : "{}");
        std::string action = a.value("action", "");
        int rc = 0;
        std::string out;

        if (action == "help" || action.empty()) {
            response = json{
                {"name", "phone_toolkit"},
                {"actions", {
                    "device_info", "battery", "clipboard_get", "clipboard_set",
                    "notify", "camera", "location", "sensors"
                }},
                {"examples", {
                    {{"action","device_info"}},
                    {{"action","clipboard_get"}},
                    {{"action","clipboard_set"},{"text","Hello from Dibs"}},
                    {{"action","notify"},{"title","Dibs"},{"content","Task finished"}},
                    {{"action","camera"},{"camera_id",0}},
                    {{"action","location"},{"provider","gps"}},
                    {{"action","sensors"},{"sensor","all"},{"count",1}}
                }},
                {"note", "Requires Termux:API app and the termux-api package; Android permissions may be requested."}
            }.dump();
            return response.c_str();
        }

        if (action == "device_info") {
            json result;
            result["manufacturer"] = run_cmd("getprop ro.product.manufacturer");
            result["model"] = run_cmd("getprop ro.product.model");
            result["android_version"] = run_cmd("getprop ro.build.version.release");
            result["sdk"] = run_cmd("getprop ro.build.version.sdk");
            result["device"] = run_cmd("getprop ro.product.device");
            result["storage"] = run_cmd("df -h /data");
            result["battery_raw"] = run_cmd("termux-battery-status", &rc);
            if (rc != 0) result["battery_error"] = result["battery_raw"];
            response = result.dump();
            return response.c_str();
        }

        if (action == "battery") {
            out = run_cmd("termux-battery-status", &rc);
        } else if (action == "clipboard_get") {
            out = run_cmd("termux-clipboard-get", &rc);
        } else if (action == "clipboard_set") {
            if (!a.contains("text") || !a["text"].is_string()) {
                response = error_json("Missing string field: text").dump();
                return response.c_str();
            }
            out = run_cmd("termux-clipboard-set " + shell_quote(a["text"].get<std::string>()), &rc);
        } else if (action == "notify") {
            std::string title = a.value("title", "Dibs");
            std::string content = a.value("content", "Notification from Dibs");
            std::string cmd = "termux-notification --title " + shell_quote(title) +
                              " --content " + shell_quote(content);
            if (a.contains("id")) cmd += " --id " + std::to_string(a["id"].get<int>());
            out = run_cmd(cmd, &rc);
        } else if (action == "camera") {
            int camera_id = a.value("camera_id", 0);
            std::string path = "/data/data/com.termux/files/home/dibs_camera.jpg";
            out = run_cmd("termux-camera-photo -c " + std::to_string(camera_id) + " " +
                          shell_quote(path), &rc);
            if (rc != 0) {
                response = error_json("Camera capture failed: " + out).dump();
                return response.c_str();
            }
            std::string bytes = read_file(path);
            if (bytes.empty()) {
                response = error_json("Camera command completed but image file is empty or unreadable: " + path).dump();
                return response.c_str();
            }
            json envelope = {
                {"_dibs_mcp_content", json::array({
                    {{"type","text"},{"text","Camera image captured successfully."}},
                    {{"type","image"},{"data",base64_encode(bytes)},{"mimeType","image/jpeg"}}
                })},
                {"_dibs_is_error", false}
            };
            response = envelope.dump();
            return response.c_str();
        } else if (action == "location") {
            std::string provider = a.value("provider", "gps");
            if (provider != "gps" && provider != "network" && provider != "passive") {
                response = error_json("provider must be gps, network, or passive").dump();
                return response.c_str();
            }
            out = run_cmd("termux-location -p " + provider + " -r once", &rc);
        } else if (action == "sensors") {
            std::string sensor = a.value("sensor", "all");
            int count = a.value("count", 1);
            if (count < 1 || count > 20) count = 1;
            out = run_cmd("termux-sensor -s " + shell_quote(sensor) + " -n " +
                          std::to_string(count), &rc);
        } else {
            response = error_json("Unknown action. Use action=help.").dump();
            return response.c_str();
        }

        if (rc != 0) {
            response = error_json(out.empty() ? "Command failed" : out).dump();
        } else {
            response = json{{"action", action}, {"result", out}}.dump();
        }
        return response.c_str();
    } catch (const std::exception& e) {
        response = json{{"error", e.what()}, {"isError", true}}.dump();
        return response.c_str();
    }
}
