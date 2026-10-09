// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#pragma once
#include <filesystem>
#include <string>

struct Config {
	std::string ws_url = "ws://127.0.0.1:3001";
	std::string access_token;
	std::string core_path;  // empty = auto-detect
	std::string root_dir;   // empty = folder of dice-host
	int workers = 4;
	int rpc_timeout_ms = 5000;
	int list_cache_sec = 600;
	int member_cache_sec = 300;
	bool strip_reply = true;
	bool forward_poke = true;
	bool forward_notice = false;
	bool media_base64 = false;
	std::string log_level = "info";
	bool log_to_file = true;
	bool restart_on_crash = true;

	// Resolved values (absolute, UTF-8 aware paths)
	std::filesystem::path config_file;
	std::filesystem::path root;
	std::filesystem::path core;
};

// Loads the config file, creating a commented default one when it does not exist.
// Returns false (with `err`) when the file exists but cannot be parsed.
bool load_config(const std::filesystem::path& file, Config& cfg, std::string& err, bool& created);

// Locate the Dice! core library when `core_path` is empty or missing.
std::filesystem::path find_core(const Config& cfg, std::string& note);
