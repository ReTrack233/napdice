// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#include "platform.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace plat {

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}
static std::string narrow(const std::wstring& w, UINT cp, bool* lossless = nullptr) {
	if (lossless) *lossless = true;
	if (w.empty()) return {};
	BOOL usedDefault = FALSE;
	const bool canCheck = lossless && cp != CP_UTF8 && cp != CP_UTF7;
	DWORD flags = canCheck ? WC_NO_BEST_FIT_CHARS : 0;
	int n = WideCharToMultiByte(cp, flags, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr,
								canCheck ? &usedDefault : nullptr);
	std::string s(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(cp, flags, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
						canCheck ? &usedDefault : nullptr);
	if (lossless && usedDefault) *lossless = false;
	return s;
}
#endif

fs::path exe_path() {
#ifdef _WIN32
	std::wstring buf(MAX_PATH, L'\0');
	for (;;) {
		DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
		if (n == 0) return {};
		if (n < buf.size()) {
			buf.resize(n);
			break;
		}
		buf.resize(buf.size() * 2);
	}
	return fs::path(buf);
#else
	std::error_code ec;
	auto p = fs::read_symlink("/proc/self/exe", ec);
	return ec ? fs::path() : p;
#endif
}

fs::path exe_dir() { return exe_path().parent_path(); }

fs::path u8path(const std::string& s) {
#ifdef _WIN32
	return fs::path(widen(s));
#else
	return fs::path(s);
#endif
}

std::string path_u8(const fs::path& p) {
#ifdef _WIN32
	return narrow(p.wstring(), CP_UTF8);
#else
	return p.string();
#endif
}

std::string utf8_to_native(const std::string& s, bool& lossless) {
	lossless = true;
#ifdef _WIN32
	if (GetACP() == CP_UTF8) return s;
	return narrow(widen(s), CP_ACP, &lossless);
#else
	return s;
#endif
}

void* lib_open(const fs::path& p, std::string& err) {
#ifdef _WIN32
	// Keep the loader from showing a "DLL not found / bad image / entry point" message box. With no one
	// to click it (a service, a remote session, CI) it would hang the host forever while holding the
	// loader lock; with this set, LoadLibrary fails with an error code we can report instead.
	DWORD oldMode = 0;
	const bool modeSet = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &oldMode) != FALSE;
	HMODULE h = LoadLibraryExW(p.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	const DWORD code = h ? 0 : GetLastError();
	if (modeSet) SetThreadErrorMode(oldMode, nullptr);
	if (!h) {
		wchar_t* msg = nullptr;
		FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					   nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
		err = "LoadLibrary 失败 (错误码 " + std::to_string(code) + ")";
		if (msg) {
			err += ": " + narrow(msg, CP_UTF8);
			LocalFree(msg);
		}
		while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
		return nullptr;
	}
	return reinterpret_cast<void*>(h);
#else
	void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		const char* e = dlerror();
		err = e ? e : "dlopen failed";
	}
	return h;
#endif
}

void* lib_sym(void* handle, const char* name) {
#ifdef _WIN32
	return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
	return dlsym(handle, name);
#endif
}

int pe_bits(const fs::path& p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return -1;
	unsigned char dos[0x40]{};
	if (!f.read(reinterpret_cast<char*>(dos), sizeof(dos))) return 0;
	if (dos[0] != 'M' || dos[1] != 'Z') return 0;
	uint32_t off = dos[0x3C] | (dos[0x3D] << 8) | (dos[0x3E] << 16) | (static_cast<uint32_t>(dos[0x3F]) << 24);
	f.seekg(off);
	unsigned char hdr[6]{};
	if (!f.read(reinterpret_cast<char*>(hdr), sizeof(hdr))) return 0;
	if (hdr[0] != 'P' || hdr[1] != 'E' || hdr[2] || hdr[3]) return 0;
	uint16_t machine = static_cast<uint16_t>(hdr[4] | (hdr[5] << 8));
	if (machine == 0x014c) return 32;  // IMAGE_FILE_MACHINE_I386
	if (machine == 0x8664) return 64;  // IMAGE_FILE_MACHINE_AMD64
	return 0;
}

void init_console() {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
#else
	signal(SIGPIPE, SIG_IGN);
#endif
}

static std::atomic<bool>* g_stop_flag = nullptr;
static void (*g_close_wait)() = nullptr;

#ifdef _WIN32
static BOOL WINAPI console_handler(DWORD type) {
	if (g_stop_flag) g_stop_flag->store(true);
	switch (type) {
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		// Windows terminates the process as soon as this handler returns, so finish cleanup here.
		if (g_close_wait) g_close_wait();
		return TRUE;
	default:
		return TRUE;
	}
}
#else
static void signal_handler(int) {
	if (g_stop_flag) g_stop_flag->store(true);
}
#endif

void install_stop_handler(std::atomic<bool>* flag, void (*on_close_wait)()) {
	g_stop_flag = flag;
	g_close_wait = on_close_wait;
#ifdef _WIN32
	SetConsoleCtrlHandler(console_handler, TRUE);
#else
	struct sigaction sa {};
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGHUP, &sa, nullptr);
#endif
}

#ifdef _WIN32
// Quote one argument following the CommandLineToArgvW rules.
static std::wstring quote_arg(const std::wstring& a) {
	if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
	std::wstring r = L"\"";
	for (auto it = a.begin();; ++it) {
		size_t backslashes = 0;
		while (it != a.end() && *it == L'\\') {
			++it;
			++backslashes;
		}
		if (it == a.end()) {
			r.append(backslashes * 2, L'\\');
			break;
		}
		if (*it == L'"') {
			r.append(backslashes * 2 + 1, L'\\');
			r.push_back(*it);
		}
		else {
			r.append(backslashes, L'\\');
			r.push_back(*it);
		}
	}
	r.push_back(L'"');
	return r;
}
#endif

int run_child(const std::vector<std::string>& args, std::atomic<bool>& stop) {
	using namespace std::chrono;
	const fs::path self = exe_path();
#ifdef _WIN32
	std::wstring cmd = quote_arg(self.wstring());
	for (const auto& a : args) cmd += L" " + quote_arg(widen(a));
	// Hand our stdin/stdout/stderr to the worker explicitly, so its log reaches the same place
	// whether we run in a console window or with redirected output (pipes, files, services).
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	for (HANDLE h : {si.hStdInput, si.hStdOutput, si.hStdError}) {
		if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	}
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(self.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
		return -1;
	}
	CloseHandle(pi.hThread);
	auto stopSince = steady_clock::time_point{};
	for (;;) {
		if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
		if (stop.load()) {
			// The child shares our console and receives Ctrl+C / close events itself.
			if (stopSince == steady_clock::time_point{}) stopSince = steady_clock::now();
			else if (steady_clock::now() - stopSince > seconds(40)) {
				TerminateProcess(pi.hProcess, 1);
				WaitForSingleObject(pi.hProcess, 5000);
				break;
			}
		}
	}
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return static_cast<int>(code);
#else
	std::vector<std::string> argStore;
	argStore.push_back(self.string());
	for (const auto& a : args) argStore.push_back(a);
	std::vector<char*> argv;
	for (auto& s : argStore) argv.push_back(s.data());
	argv.push_back(nullptr);
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		execv(argv[0], argv.data());
		_exit(127);
	}
	auto stopSince = steady_clock::time_point{};
	int status = 0;
	for (;;) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) break;
		if (r < 0) return -1;
		if (stop.load()) {
			if (stopSince == steady_clock::time_point{}) {
				stopSince = steady_clock::now();
				kill(pid, SIGTERM);
			}
			else if (steady_clock::now() - stopSince > seconds(40)) {
				kill(pid, SIGKILL);
			}
		}
		std::this_thread::sleep_for(milliseconds(200));
	}
	if (WIFEXITED(status)) return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return -1;
#endif
}

std::string now_str(const char* fmt) {
	std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char buf[64];
	size_t n = std::strftime(buf, sizeof(buf), fmt, &tm);
	return std::string(buf, n);
}

} // namespace plat
