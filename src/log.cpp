// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#include "log.hpp"

#include <cstdio>
#include <fstream>
#include <mutex>

#include "platform.hpp"

namespace logx {

static std::mutex g_mu;
static Level g_level = Level::info;
static std::filesystem::path g_dir;
static std::string g_tag;
static std::ofstream g_file;
static std::string g_fileDate;

Level parse_level(const std::string& s) {
	if (s == "debug") return Level::debug;
	if (s == "warn" || s == "warning") return Level::warn;
	if (s == "error") return Level::error;
	return Level::info;
}

void init(Level level, const std::filesystem::path& dir, const std::string& tag) {
	std::lock_guard<std::mutex> lk(g_mu);
	g_level = level;
	g_dir = dir;
	g_tag = tag;
	if (!g_dir.empty()) {
		std::error_code ec;
		std::filesystem::create_directories(g_dir, ec);
	}
}

static const char* level_name(Level l) {
	switch (l) {
	case Level::debug: return "DEBUG";
	case Level::info: return "INFO ";
	case Level::warn: return "WARN ";
	default: return "ERROR";
	}
}

void write(Level level, const std::string& msg) {
	if (level < g_level) return;
	std::string line = plat::now_str("%Y-%m-%d %H:%M:%S") + " [" + level_name(level) + "]" +
		(g_tag.empty() ? "" : " [" + g_tag + "]") + " " + msg + "\n";
	std::lock_guard<std::mutex> lk(g_mu);
	FILE* out = level >= Level::warn ? stderr : stdout;
	std::fwrite(line.data(), 1, line.size(), out);
	std::fflush(out);
	if (!g_dir.empty()) {
		std::string date = plat::now_str("%Y-%m-%d");
		if (date != g_fileDate || !g_file.is_open()) {
			if (g_file.is_open()) g_file.close();
			g_file.open(g_dir / ("dice-host-" + date + ".log"), std::ios::app | std::ios::binary);
			g_fileDate = date;
		}
		if (g_file) {
			g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
			g_file.flush();
		}
	}
}

} // namespace logx
