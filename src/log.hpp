// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#pragma once
#include <filesystem>
#include <string>

namespace logx {

enum class Level { debug = 0, info = 1, warn = 2, error = 3 };

Level parse_level(const std::string& s);
// `dir` may be empty to disable file logging. `tag` is printed in front of every line.
void init(Level level, const std::filesystem::path& dir, const std::string& tag);
void write(Level level, const std::string& msg);

inline void debug(const std::string& m) { write(Level::debug, m); }
inline void info(const std::string& m) { write(Level::info, m); }
inline void warn(const std::string& m) { write(Level::warn, m); }
inline void error(const std::string& m) { write(Level::error, m); }

} // namespace logx
