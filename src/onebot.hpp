// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
//
// OneBot 11 client over a forward WebSocket: request/response correlation by `echo`,
// automatic reconnect, liveness probing.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "nlohmann/json.hpp"

class WsClient;

class OneBot {
public:
	struct Result {
		bool ok = false;
		int retcode = -1;
		nlohmann::json data;
		std::string message;
	};
	using EventFn = std::function<void(nlohmann::json&&)>;
	using StateFn = std::function<void(bool connected)>;

	OneBot(std::string url, std::string token);
	~OneBot();

	void set_on_event(EventFn f) { onEvent_ = std::move(f); }
	// Called from the connection thread right after (re)connecting and after disconnecting.
	void set_on_state(StateFn f) { onState_ = std::move(f); }

	void start();
	void stop();
	bool connected() const { return connected_.load(); }
	bool wait_connected(int timeout_ms);

	// Blocking call. Never throws.
	Result call(const std::string& action, nlohmann::json params, int timeout_ms);
	// Fire and forget; failures are logged.
	void post(const std::string& action, nlohmann::json params);

private:
	struct Pending {
		std::mutex m;
		std::condition_variable cv;
		bool done = false;
		bool fire = false;
		std::string action;
		std::chrono::steady_clock::time_point start;
		Result r;
	};
	bool send_request(const std::string& action, nlohmann::json&& params, const std::shared_ptr<Pending>& p);
	void manager_loop();
	void handle_text(std::string&& text);
	void fail_all_pending(const std::string& why);
	void sweep_pending();

	std::string url_, token_;
	EventFn onEvent_;
	StateFn onState_;
	std::atomic<bool> stopping_{false};
	std::atomic<bool> connected_{false};
	std::atomic<bool> dropped_{false};
	std::atomic<long long> lastRecvMs_{0};
	std::atomic<unsigned long long> seq_{0};
	std::mutex wsMu_;
	std::unique_ptr<WsClient> ws_;
	std::mutex stateMu_;
	std::condition_variable stateCv_;
	std::mutex pendMu_;
	std::unordered_map<std::string, std::shared_ptr<Pending>> pending_;
	std::thread mgr_;
};
