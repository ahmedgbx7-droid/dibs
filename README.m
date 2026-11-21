# Dibs

A powerful C++ MCP (Model Context Protocol) Server for Termux/Linux.

## Features
- Run shell commands safely with a 15s timeout.
- File system management (read, write, list).
- System info (battery, memory).
- Web fetching.
- Auto-compiling extensions (drop .cpp file in extensions/ folder).

## Build Instructions
mkdir build && cd build
cmake ..
make

## Usage
./dibs

> Warning: This tool grants full shell access. Bind to 127.0.0.1 only.