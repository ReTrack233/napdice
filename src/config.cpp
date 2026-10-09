// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

#include "nlohmann/json.hpp"
#include "platform.hpp"

using nlohmann::json;
namespace fs = std::filesystem;

static const char* kDefaultConfig = R"(// dice-host 配置文件（JSON，允许 // 注释）
{
  // NapCat 的 OneBot 11 正向 WebSocket 地址（NapCat WebUI → 网络配置 → 新建 WebSocket 服务器）
  "ws_url": "ws://127.0.0.1:3001",
  // 与 NapCat 中填写的 Token 一致；没设 Token 就留空
  "access_token": "",

  // Dice! 核心 DLL 路径。留空则自动查找：dice-host 同目录、root_dir、root_dir/Diceki 下的 w4123.Dice*.dll / Dice*.dll
  "core_path": "",
  // 骰子根目录：其下的 Dice{QQ号}/ 是骰娘数据，Diceki/ 放公共脚本。留空 = dice-host 所在目录
  // 从 DiceDriver.OneBot 迁移时，填原来 DiceDriver.OneBot.exe 所在的文件夹即可
  "root_dir": "",

  // 处理事件的工作线程数（同一个群/私聊的消息始终按顺序处理）
  "workers": 4,
  // 向 NapCat 查询（成员列表、昵称等）的超时，毫秒
  "rpc_timeout_ms": 5000,
  // 好友列表 / 群列表缓存时间，秒
  "list_cache_sec": 600,
  // 群成员信息缓存时间，秒
  "member_cache_sec": 300,

  // 回复别人消息时 QQ 会在开头带上引用，去掉它，否则骰娘认不出 "回复+@骰娘+指令"
  "strip_reply": true,
  // 把戳一戳转成 Dice! 的 Poke 事件（mod 里用 "event": "Poke" 的 hook 接）
  "forward_poke": true,
  // 把其他 OneBot 通知转成 Dice! 的 OneBotNotice 事件
  "forward_notice": false,
  // NapCat 和 dice-host 不在同一台机器 / NapCat 跑在 Docker 里时设为 true：图片、语音、文件改用 base64 发送
  "media_base64": false,

  // 日志级别：debug / info / warn / error
  "log_level": "info",
  // 同时写入 logs/ 目录
  "log_to_file": true,
  // 骰娘进程意外退出后自动重启
  "restart_on_crash": true
}
)";

template <typename T>
static void get_opt(const json& j, const char* key, T& out) {
	auto it = j.find(key);
	if (it != j.end() && !it->is_null()) {
		try {
			out = it->get<T>();
		}
		catch (...) {
		}
	}
}

bool load_config(const fs::path& file, Config& cfg, std::string& err, bool& created) {
	created = false;
	cfg.config_file = file;
	std::error_code ec;
	if (!fs::exists(file, ec)) {
		std::ofstream out(file, std::ios::binary);
		if (out) {
			out << kDefaultConfig;
			created = true;
		}
	}
	json j = json::object();
	{
		std::ifstream in(file, std::ios::binary);
		if (in) {
			std::stringstream ss;
			ss << in.rdbuf();
			std::string text = ss.str();
			if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
				static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
				text.erase(0, 3);  // UTF-8 BOM written by Notepad
			}
			try {
				j = json::parse(text, nullptr, true, true);
			}
			catch (const std::exception& e) {
				err = std::string("配置文件格式错误: ") + e.what();
				return false;
			}
		}
		else if (!created) {
			err = "无法读取配置文件";
			return false;
		}
		else {
			j = json::parse(kDefaultConfig, nullptr, true, true);
		}
	}
	get_opt(j, "ws_url", cfg.ws_url);
	get_opt(j, "access_token", cfg.access_token);
	get_opt(j, "core_path", cfg.core_path);
	get_opt(j, "root_dir", cfg.root_dir);
	get_opt(j, "workers", cfg.workers);
	get_opt(j, "rpc_timeout_ms", cfg.rpc_timeout_ms);
	get_opt(j, "list_cache_sec", cfg.list_cache_sec);
	get_opt(j, "member_cache_sec", cfg.member_cache_sec);
	get_opt(j, "strip_reply", cfg.strip_reply);
	get_opt(j, "forward_poke", cfg.forward_poke);
	get_opt(j, "forward_notice", cfg.forward_notice);
	get_opt(j, "media_base64", cfg.media_base64);
	get_opt(j, "log_level", cfg.log_level);
	get_opt(j, "log_to_file", cfg.log_to_file);
	get_opt(j, "restart_on_crash", cfg.restart_on_crash);
	cfg.workers = std::clamp(cfg.workers, 1, 64);
	cfg.rpc_timeout_ms = std::clamp(cfg.rpc_timeout_ms, 500, 120000);
	cfg.list_cache_sec = std::max(cfg.list_cache_sec, 10);
	cfg.member_cache_sec = std::max(cfg.member_cache_sec, 10);

	const fs::path base = plat::exe_dir();
	cfg.root = cfg.root_dir.empty() ? base : plat::u8path(cfg.root_dir);
	if (cfg.root.is_relative()) cfg.root = base / cfg.root;
	cfg.root = fs::weakly_canonical(cfg.root, ec);
	if (ec) cfg.root = cfg.root_dir.empty() ? base : base / plat::u8path(cfg.root_dir);
	if (!cfg.core_path.empty()) {
		cfg.core = plat::u8path(cfg.core_path);
		if (cfg.core.is_relative()) cfg.core = base / cfg.core;
	}
	return true;
}

static bool looks_like_core(const fs::path& p) {
	std::string name = plat::path_u8(p.filename());
	std::string lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#ifdef _WIN32
	const std::string ext = ".dll";
#else
	const std::string ext = ".so";
#endif
	bool extOk = lower.size() > ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0;
#ifndef _WIN32
	// Dice! builds keep the .dll suffix on every platform
	extOk = extOk || (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".dll") == 0);
#endif
	if (!extOk) return false;
	return lower.rfind("w4123.dice", 0) == 0 || lower.rfind("dice", 0) == 0;
}

fs::path find_core(const Config& cfg, std::string& note) {
	std::error_code ec;
	if (!cfg.core.empty()) {
		if (fs::exists(cfg.core, ec)) return cfg.core;
		note = "core_path 指向的文件不存在: " + plat::path_u8(cfg.core);
		return {};
	}
	std::vector<fs::path> dirs{plat::exe_dir(), cfg.root, cfg.root / "Diceki"};
	std::vector<fs::path> found;
	for (const auto& d : dirs) {
		if (!fs::is_directory(d, ec)) continue;
		std::vector<fs::path> here;
		for (const auto& e : fs::directory_iterator(d, ec)) {
			if (e.is_regular_file(ec) && looks_like_core(e.path())) here.push_back(e.path());
		}
		// Prefer the canonical name, then alphabetical
		std::sort(here.begin(), here.end(), [](const fs::path& a, const fs::path& b) {
			bool aw = plat::path_u8(a.filename()).rfind("w4123.", 0) == 0;
			bool bw = plat::path_u8(b.filename()).rfind("w4123.", 0) == 0;
			if (aw != bw) return aw;
			return a < b;
		});
		for (auto& p : here) found.push_back(p);
	}
#ifdef _WIN32
	for (const auto& p : found) {
		if (plat::pe_bits(p) == plat::self_bits()) return p;
	}
	if (!found.empty()) {
		note = "找到了核心 " + plat::path_u8(found.front()) + "，但它是 " + std::to_string(plat::pe_bits(found.front())) +
			" 位的，当前 dice-host 是 " + std::to_string(plat::self_bits()) + " 位，请换用对应位数的 dice-host";
		return {};
	}
#else
	if (!found.empty()) return found.front();
#endif
	note = "没有找到 Dice! 核心（w4123.Dice*.dll），请把它放到 dice-host 同目录或在配置里填写 core_path";
	return {};
}
