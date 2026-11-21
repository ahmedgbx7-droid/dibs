# Dibs 🥤 
**A powerful, lightweight C++ MCP (Model Context Protocol) Server for Termux/Linux.**

## Features 🚀

· Run Commands: Execute shell commands safely with a built-in 15s timeout.
· File System Management: Read, write, and list files/directories.
· System Info: Get device battery percentage, kernel info, and memory usage.
· Web Fetching: Fetch content from any URL directly through the AI.
· Auto-compiling Extensions: Drop a .cpp file into the extensions/ folder, and Dibs will automatically compile and load it on startup!

## Creating Extensions 🧩

You don't need to compile manually. Just write a .cpp file in the extensions/ folder with the required extern "C" functions, and Dibs will handle the rest when you run it. (Check the extensions/examples/ folder for a ready-to-use example).

⚠️ Security Warning: This tool grants full shell access to the host system. Always bind it to 127.0.0.1 (localhost). Do not expose this server to the public internet unless you know exactly what you are doing.