// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
//
// dice-host: loads the Dice! core and connects it to NapCat over OneBot 11 (forward WebSocket).
//
//   dice-host                 supervisor: runs a worker process and restarts it when needed
//   dice-host --worker        the actual driver (used by the supervisor; can be run directly)
//   dice-host --config FILE   use another config file (default: dice-host.json next to the exe)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "core.hpp"
#include "driver.hpp"
#include "log.hpp"
#include "onebot.hpp"
#include "platform.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#ifndef NAPDICE_VERSION
#define NAPDICE_VERSION "dev"
#endif

namespace fs = std::filesystem;
using namespace std::chrono;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFatal = 2;    // configuration / core problem: retrying won't help
constexpr int kExitRestart = 3;  // core asked for a reload, or the QQ account changed

std::atomic<bool> g_stop{false};
std::atomic<bool> g_cleanupDone{false};

void wait_cleanup_for_close() {
	// Windows gives a console app ~5s after the window is closed.
	for (int i = 0; i < 95 && !g_cleanupDone.load(); ++i) std::this_thread::sleep_for(milliseconds(50));
}

[[noreturn]] void hard_exit(int code) {
	std::fflush(stdout);
	std::fflush(stderr);
#ifdef _WIN32
	// Skip DLL static destructors of the core: its data is already saved by eventExit.
	TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#endif
	std::_Exit(code);
}

std::vector<std::string> utf8_args(int argc, char** argv) {
	std::vector<std::string> out;
#ifdef _WIN32
	(void)argc;
	(void)argv;
	int n = 0;
	LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
	for (int i = 1; w && i < n; ++i) out.push_back(plat::path_u8(fs::path(w[i])));
	if (w) LocalFree(w);
#else
	for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
#endif
	return out;
}

void pause_if_own_console() {
#ifdef _WIN32
	DWORD pids[4];
	// Exactly one process on the console = we were started by double-click and own the window.
	if (GetConsoleProcessList(pids, 4) == 1) {
		std::printf("\n按回车键退出……");
		std::fflush(stdout);
		std::getchar();
	}
#endif
}

void print_help() {
	std::printf(
		"NapDice dice-host %s — 让 Dice! 直接接入 NapCat（OneBot 11 正向 WebSocket）\n\n"
		"用法: dice-host [--config 配置文件] [--worker]\n"
		"  --config FILE  指定配置文件（默认: 程序同目录的 dice-host.json，不存在会自动生成）\n"
		"  --worker       不启用守护进程，直接运行（调试用）\n"
		"  --version      显示版本\n",
		NAPDICE_VERSION);
}

int run_worker(const fs::path& cfgPath) {
	Config cfg;
	std::string err;
	bool created = false;
	if (!load_config(cfgPath, cfg, err, created)) {
		std::fprintf(stderr, "%s\n%s\n", plat::path_u8(cfgPath).c_str(), err.c_str());
		return kExitFatal;
	}
	logx::init(logx::parse_level(cfg.log_level), cfg.log_to_file ? plat::exe_dir() / "logs" : fs::path(), "");
	logx::info(std::string("NapDice dice-host ") + NAPDICE_VERSION + "（" + std::to_string(plat::self_bits()) + " 位）");
	if (created) logx::info("已生成默认配置文件 " + plat::path_u8(cfgPath) + "，如需修改 NapCat 地址或 Token 请编辑它");

	std::string note;
	fs::path corePath = find_core(cfg, note);
	if (corePath.empty()) {
		logx::error(note);
		return kExitFatal;
	}
	logx::info("Dice! 核心: " + plat::path_u8(corePath));
	logx::info("骰子根目录: " + plat::path_u8(cfg.root));

	plat::install_stop_handler(&g_stop, &wait_cleanup_for_close);

	OneBot ob(cfg.ws_url, cfg.access_token);
	Driver drv(cfg, ob);
	ob.set_on_event([&drv](nlohmann::json&& e) { drv.on_event(std::move(e)); });
	ob.set_on_state([&drv](bool c) { drv.on_state(c); });
	logx::info("正在连接 NapCat: " + cfg.ws_url);
	ob.start();

	if (!drv.wait_login(g_stop)) {
		ob.stop();
		g_cleanupDone = true;
		return kExitOk;
	}

	DiceCore core;
	logx::info("正在加载 Dice! 核心……");
	if (!core.load(corePath, err)) {
		logx::error("加载 Dice! 核心失败: " + err);
		ob.stop();
		g_cleanupDone = true;
		return kExitFatal;
	}
	if (!drv.start_core(&core, err)) {
		logx::error(err);
		ob.stop();
		g_cleanupDone = true;
		return kExitFatal;
	}
	logx::info("骰娘已启动，关闭窗口或按 Ctrl+C 退出");

	while (!g_stop.load() && drv.request() == Driver::Request::None) std::this_thread::sleep_for(milliseconds(200));
	const int code = (!g_stop.load() && drv.request() == Driver::Request::Restart) ? kExitRestart : kExitOk;
	// Let the core's "reload finished" notice reach the send queue before we take it down.
	if (code == kExitRestart) std::this_thread::sleep_for(milliseconds(1500));

	std::thread watchdog([code] {
		for (int i = 0; i < 600 && !g_cleanupDone.load(); ++i) std::this_thread::sleep_for(milliseconds(100));
		if (!g_cleanupDone.load()) {
			logx::error("关闭核心超时，强制退出");
			hard_exit(code);
		}
	});
	drv.shutdown_core();
	ob.stop();
	g_cleanupDone = true;
	watchdog.join();
	hard_exit(code);
}

int run_supervisor(const std::vector<std::string>& passArgs, const fs::path& cfgPath) {
	logx::init(logx::Level::info, fs::path(), "守护");
	plat::install_stop_handler(&g_stop, nullptr);
	Config cfg;
	std::string err;
	bool created = false;
	if (!load_config(cfgPath, cfg, err, created)) {
		logx::error(plat::path_u8(cfgPath) + ": " + err);
		pause_if_own_console();
		return kExitFatal;
	}
	int delaySec = 5;
	for (;;) {
		auto started = steady_clock::now();
		std::vector<std::string> args{"--worker"};
		args.insert(args.end(), passArgs.begin(), passArgs.end());
		int code = plat::run_child(args, g_stop);
		if (g_stop.load()) break;
		if (code == kExitOk) break;
		if (code == kExitRestart) {
			logx::info("重新启动骰娘……");
			continue;
		}
		if (code == kExitFatal) {
			logx::error("启动失败，请根据上面的提示检查配置后再运行");
			pause_if_own_console();
			return kExitFatal;
		}
		if (code == -1) {
			logx::error("无法启动工作进程");
			pause_if_own_console();
			return 1;
		}
		logx::error("骰娘进程异常退出（退出码 " + std::to_string(code) + "）");
		if (!cfg.restart_on_crash) {
			pause_if_own_console();
			return code;
		}
		if (steady_clock::now() - started > minutes(10)) delaySec = 5;
		logx::info(std::to_string(delaySec) + " 秒后自动重启");
		for (int i = 0; i < delaySec * 10 && !g_stop.load(); ++i) std::this_thread::sleep_for(milliseconds(100));
		if (g_stop.load()) break;
		delaySec = std::min(delaySec * 2, 60);
	}
	return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
	plat::init_console();
	auto args = utf8_args(argc, argv);
	bool worker = false;
	fs::path cfgPath = plat::exe_dir() / "dice-host.json";
	std::vector<std::string> passArgs;
	for (size_t i = 0; i < args.size(); ++i) {
		const std::string& a = args[i];
		if (a == "--worker") worker = true;
		else if (a == "--config" && i + 1 < args.size()) {
			cfgPath = plat::u8path(args[++i]);
			if (cfgPath.is_relative()) cfgPath = fs::current_path() / cfgPath;
			passArgs.push_back("--config");
			passArgs.push_back(plat::path_u8(cfgPath));
		}
		else if (a == "--version" || a == "-v") {
			std::printf("NapDice dice-host %s\n", NAPDICE_VERSION);
			return 0;
		}
		else if (a == "--help" || a == "-h") {
			print_help();
			return 0;
		}
		else {
			std::fprintf(stderr, "未知参数: %s\n", a.c_str());
			print_help();
			return kExitFatal;
		}
	}
	return worker ? run_worker(cfgPath) : run_supervisor(passArgs, cfgPath);
}
