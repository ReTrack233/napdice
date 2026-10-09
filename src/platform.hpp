// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace plat {

namespace fs = std::filesystem;

// Executable location
fs::path exe_path();
fs::path exe_dir();

// UTF-8 <-> std::filesystem::path (portable; on Windows path is UTF-16 internally)
fs::path u8path(const std::string& s);
std::string path_u8(const fs::path& p);

// Convert a UTF-8 string to the narrow encoding std::filesystem uses for std::string on this
// platform (the ANSI code page on Windows, UTF-8 elsewhere). `lossless` is set to false when some
// characters cannot be represented.
std::string utf8_to_native(const std::string& s, bool& lossless);

// Dynamic library loading
void* lib_open(const fs::path& p, std::string& err);
void* lib_sym(void* handle, const char* name);

// PE header inspection: returns 32 / 64 for Windows PE images, 0 when the file is not a PE image,
// -1 when it can't be read.
int pe_bits(const fs::path& p);
constexpr int self_bits() { return static_cast<int>(sizeof(void*) * 8); }

// Console setup (UTF-8 output on Windows)
void init_console();

// Shutdown signals (Ctrl+C, console close, SIGTERM). The flag is set when a stop is requested.
// `on_close_wait` is invoked from the Windows console-close handler so the process can finish
// cleanup before Windows terminates it.
void install_stop_handler(std::atomic<bool>* flag, void (*on_close_wait)());

// Spawn the current executable with `args` and wait for it to exit. While waiting, if `stop`
// becomes true the child is asked to terminate gracefully (it also receives console signals on
// its own). Returns the child's exit code, or -1 if it could not be started.
int run_child(const std::vector<std::string>& args, std::atomic<bool>& stop);

// Current local time formatted with strftime pattern
std::string now_str(const char* fmt);

} // namespace plat
