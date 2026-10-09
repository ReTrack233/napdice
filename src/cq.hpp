// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
//
// CQ code helpers: adapt Dice! output to what NapCat understands and normalize input.
#pragma once
#include <filesystem>
#include <string>

namespace cq {

std::string escape_param(const std::string& s);
std::string unescape(const std::string& s);

// Incoming: optionally drop a leading [CQ:reply,...] (QQ adds it when replying), and reduce
// [CQ:at,qq=N,...] to [CQ:at,qq=N] so Dice! recognises @mentions.
std::string normalize_incoming(const std::string& raw, bool strip_reply);

struct MediaDirs {
	std::filesystem::path root;  // dice-host root dir (contains data/image, data/record)
	std::filesystem::path dice;  // root/Dice{QQ}
	bool base64 = false;         // embed local files as base64:// (NapCat on another machine / in Docker)
};

// Read a local file and return "base64://..." (empty string on failure).
std::string file_to_base64_uri(const std::filesystem::path& p);

// Outgoing: [CQ:at,id=N] -> [CQ:at,qq=N]; relative media paths -> absolute paths (or base64).
std::string rewrite_outgoing(const std::string& msg, const MediaDirs& dirs);

} // namespace cq
