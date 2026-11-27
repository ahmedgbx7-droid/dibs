# Dibs 🥤

**A lightweight C++ [MCP](https://modelcontextprotocol.io) (Model Context Protocol) server for Termux and Linux.**

Dibs exposes your device to an AI client as a set of tools: run shell commands, read and write files, fetch URLs, inspect system state, and load your own tools as C++ plugins that are compiled automatically on startup.

> ⚠️ **Security:** Dibs grants **full shell and filesystem access** to whoever can reach it, and it has **no authentication**. Keep it bound to `127.0.0.1` (the default) and never expose it to the public internet. See [Security](#security).

---

## Table of Contents

- [Features](#features)
- [Requirements](#requirements)
- [Build](#build)
- [Run](#run)
- [Configuration](#configuration)
- [Remote Access](#remote-access)
- [Built-in Tools](#built-in-tools)
- [Bundled Extensions](#bundled-extensions)
- [Writing an Extension](#writing-an-extension)
- [Returning Images from an Extension](#returning-images-from-an-extension)
- [Testing the Server](#testing-the-server)
- [Security](#security)
- [Project Layout](#project-layout)
- [License](#license)

---

## Features

- **MCP over HTTP**: JSON-RPC 2.0 on `POST /mcp` (protocol version `2024-11-05`).
- **Shell access**: run commands with a 60-second timeout.
- **Filesystem tools**: read, write and list files and directories.
- **Web fetching**: retrieve content from any HTTP/HTTPS URL.
- **Device info**: battery level, CPU temperature, kernel, disk and memory usage.
- **Dev helpers**: recursive code search (`grep`) and crash backtraces (`gdb`).
- **Auto-compiling extensions**: drop a `.cpp` file into `extensions/`, and Dibs builds and loads it at startup. Rebuilds happen only when the source is newer than the `.so`.
- **Image results**: extensions can return images (for example a camera photo or a screenshot) to the AI client.

## Requirements

| Dependency | Purpose |
|---|---|
| C++17 compiler (`clang++` or `g++`) | Build Dibs and its extensions |
| CMake ≥ 3.14 | Build system |
| `git` + internet access | CMake downloads [cpp-httplib](https://github.com/yhirose/cpp-httplib) v0.15.3 on first configure |
| [nlohmann-json](https://github.com/nlohmann/json) | JSON handling |
| `gdb` *(optional)* | Used by the `debug_binary` tool |
| Termux:API app + `termux-api` package *(optional)* | Used by `speak`, `phone_toolkit`, and the notification example |
| `libvncclient` *(optional)* | Required by the `vnc_control` extension |

**Termux**

```bash
pkg install clang cmake git nlohmann-json termux-api gdb
```

**Debian / Ubuntu**

```bash
sudo apt install build-essential clang cmake git nlohmann-json3-dev gdb
```

## Build

```bash
git clone https://github.com/ahmedgbx7-droid/dibs.git
cd dibs
mkdir build && cd build
cmake ..
make
```

## Run

Run the binary **from the `build/` directory** so the default extensions path (`../extensions`) resolves correctly:

```bash
./dibs
```

Expected output:

```
========================================
Dibs Mega MCP Server v1.2.0 is running!
HTTP Endpoint:  http://127.0.0.1:8080/mcp
========================================
```

Point your MCP client at `http://127.0.0.1:8080/mcp`.

## Configuration

All settings are optional environment variables.

| Variable | Default | Description |
|---|---|---|
| `DIBS_HOST` | `127.0.0.1` | Address to bind to. Change only if you understand the [risks](#security). |
| `DIBS_PORT` | `8080` | Port to listen on. |
| `DIBS_TOKEN` | *(none)* | Secret token. Required automatically when not bound to localhost. |
| `DIBS_EXTENSIONS_DIR` | `../extensions` | Folder scanned for extensions. |
| `DIBS_CXX` | `clang++` | Compiler used to build extensions (for example `g++`). |

Example:

```bash
DIBS_CXX=g++ DIBS_PORT=9000 ./dibs
```

Command-line flags are listed in [Remote Access](#remote-access); `dibs --help` shows them all.

## Remote Access

By default Dibs only listens on `127.0.0.1` (this device). When you start it in a terminal it asks you two questions:

```
=== Dibs setup ===
Allow other devices on your network (same Wi-Fi) to use Dibs? [y/N]:
Open Dibs to the internet with a public HTTPS link? [y/N]:
```

Your answers are applied immediately. Nothing extra is started unless you answer yes, so the default run uses no additional memory.

**1. Network access (same Wi-Fi / LAN).** Dibs binds to `0.0.0.0` and **generates a secret token automatically** (it never runs unauthenticated on a network). Use the printed address from the other device:

```
http://<device-ip>:8080/mcp/<token>
```

Find the IP with `ifconfig` or `ip addr`. The token can also be sent as `Authorization: Bearer <token>`. This is plain HTTP, so use it only on networks you trust.

**2. Internet link (public HTTPS).** Dibs starts a free Cloudflare quick tunnel and prints a public URL:

```
https://random-words.trycloudflare.com/mcp/<token>
```

Install it first: `pkg install cloudflared` (Termux) or see the [cloudflared releases](https://github.com/cloudflare/cloudflared/releases). No domain, account or router settings are needed. The tunnel is a child process that stops when Dibs stops, and the link changes on every start.

> **Why a public link?** Cloud clients such as claude.ai connect from Anthropic's servers, not from your device, so they can only reach a public HTTPS address. A local address like `127.0.0.1` or a LAN IP cannot work for them. Clients running on the same device or computer (Claude Desktop, Claude Code) can use `http://127.0.0.1:8080/mcp` directly with no tunnel.

**Skip the questions** with flags:

| Flag | Effect |
|---|---|
| `--local` | This device only, no questions |
| `--remote` | Allow the network, skip that question |
| `--tunnel` | Open the public link, skip that question |
| `--no-tunnel` | Never open a public link |
| `--host`, `--port`, `--token` | Manual control |

When Dibs is not run from a terminal (for example as a service), it does not ask and stays local-only unless you pass flags.

## Built-in Tools

| Tool | Arguments | Description |
|---|---|---|
| `run_command` | `command` | Run a shell command (60 s timeout, stdout and stderr combined). |
| `read_file` | `path` | Read a file. |
| `write_file` | `path`, `content` | Write (overwrite) a file. |
| `list_dir` | `path` | List a directory. |
| `fetch_url` | `url` | HTTP/HTTPS GET (5 s connect, 10 s read timeout). |
| `get_battery` | none | Battery percentage from `/sys/class/power_supply`. |
| `get_thermal` | none | CPU temperature in °C from `thermal_zone0`. |
| `sys_info` | none | `uname`, disk usage of `/data`, and memory usage. |
| `search_code` | `path`, `pattern` | Recursive `grep -rn`. |
| `debug_binary` | `binary_path`, `args` *(optional)* | Run a binary under `gdb` and return the backtrace on a crash. |

> `get_battery`, `get_thermal` and `sys_info` read Android/Linux paths. On some devices these files may be unavailable or need different permissions.

## Bundled Extensions

| Tool name | Source | Description |
|---|---|---|
| `get_time` | `extensions/time.cpp` | Current system date and time. |
| `speak` | `extensions/speak.cpp` | Speak text aloud with `termux-tts-speak`. Args: `{text}`. |
| `phone_toolkit` | `extensions/dibs_phone_toolkit.cpp` | Device info, battery, clipboard, notifications, camera, location and sensors via Termux:API. Call with `{"action": "help"}` to list actions. |
| `vnc_control` | *(prebuilt `vnc_control.so` only)* | Control a remote VNC server: `get_resolution`, `screenshot`, `click`, `swipe`, `type`. Requires `libvncclient`. |
| `send_notification` | `extensions/examples/notification.cpp` | Example extension. Sends an Android notification. |

## Writing an Extension

An extension is a single `.cpp` file that exports three C functions:

```cpp
#include <string>
#include <nlohmann/json.hpp>

// Tool name: lowercase letters, digits, "_" and "-" only.
extern "C" const char* dibs_extension_name() { return "hello"; }

// Description shown to the AI. Explain what the tool does and its arguments.
extern "C" const char* dibs_extension_desc() {
    return "Greets a person. Args: {\"name\": string}";
}

// Receives the arguments as a JSON string and returns the result text.
extern "C" const char* dibs_extension_exec(const char* args_json) {
    static std::string response;   // must outlive the call
    try {
        auto args = nlohmann::json::parse(args_json ? args_json : "{}");
        response = "Hello, " + args.value("name", "world") + "!";
    } catch (const std::exception& e) {
        response = std::string("Error: ") + e.what();
    }
    return response.c_str();
}
```

1. Save it as `extensions/hello.cpp`.
2. Restart Dibs. It compiles `hello.so` automatically and loads it.
3. The tool `hello` now appears in `tools/list`.

**Rules and tips**

- The returned pointer must stay valid after the function returns. Use a `static std::string`.
- Tool names must match `[a-z0-9_-]+`, otherwise the extension is skipped.
- **Always quote user input before putting it in a shell command.** See `speak.cpp` and `examples/notification.cpp` for a safe `shell_quote` helper.
- Compile errors are printed to the server's stderr; a failed extension is skipped and the server keeps running.
- To compile manually: `clang++ -std=c++17 -shared -fPIC -O2 hello.cpp -o hello.so`

## Returning Images from an Extension

Return a JSON object with a `_dibs_mcp_content` array. Dibs validates it and forwards it to the client:

```json
{
  "_dibs_mcp_content": [
    { "type": "text",  "text": "Screenshot captured." },
    { "type": "image", "data": "<base64>", "mimeType": "image/png" }
  ],
  "_dibs_is_error": false
}
```

Only `text` items and `image/*` items are accepted. See the `camera` action in `dibs_phone_toolkit.cpp` for a complete example.

## Testing the Server

List the available tools:

```bash
curl -s http://127.0.0.1:8080/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

Call a tool:

```bash
curl -s http://127.0.0.1:8080/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"run_command","arguments":{"command":"uname -a"}}}'
```

## Security

Dibs is a remote-control tool by design. Treat it accordingly.

- **Default bind is `127.0.0.1`.** Only apps on the same device can reach it.
- **A token is always required** for network access or a public link (auto-generated if you don't pass one).
- **In local mode there is no authentication.** Anyone who can reach the port can run arbitrary commands as your user, read and overwrite your files, and use the camera, clipboard and location through `phone_toolkit`.
- **CORS is open (`*`)**, so a web page opened on the same device can talk to a local Dibs instance. Do not browse untrusted sites while Dibs runs unless you accept that.
- **`fetch_url` does not verify TLS certificates.**
- If you must expose it beyond localhost, put it behind an authenticated tunnel or reverse proxy (SSH tunnel, WireGuard, Tailscale, or an HTTPS proxy with a token). Do not enable network access on an untrusted network, since traffic is plain HTTP and the token could be sniffed.
- Extensions run inside the Dibs process with full privileges. Only load code you trust.

## Project Layout

```
dibs/
├── CMakeLists.txt
├── LICENSE
├── README.md
├── src/
│   └── main.cpp                  # MCP server and built-in tools
└── extensions/
    ├── time.cpp                  # get_time
    ├── speak.cpp                 # speak
    ├── dibs_phone_toolkit.cpp    # phone_toolkit
    └── examples/
        └── notification.cpp      # example extension
```

Compiled `.so` files and the `build/` directory are git-ignored.

## License

Dibs is released under the [GNU General Public License v3.0](LICENSE).
