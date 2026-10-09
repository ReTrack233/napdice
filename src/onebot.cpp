// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#include "onebot.hpp"

#include <vector>

#include "log.hpp"
#include "ws_client.hpp"

using nlohmann::json;
using namespace std::chrono;

static long long now_ms() {
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

OneBot::OneBot(std::string url, std::string token) : url_(std::move(url)), token_(std::move(token)) {}

OneBot::~OneBot() { stop(); }

void OneBot::start() {
	stopping_ = false;
	mgr_ = std::thread([this] { manager_loop(); });
}

void OneBot::stop() {
	stopping_ = true;
	stateCv_.notify_all();
	if (mgr_.joinable()) mgr_.join();
}

bool OneBot::wait_connected(int timeout_ms) {
	std::unique_lock<std::mutex> lk(stateMu_);
	return stateCv_.wait_for(lk, milliseconds(timeout_ms), [this] { return connected_.load() || stopping_.load(); }) &&
		connected_.load();
}

bool OneBot::send_request(const std::string& action, json&& params, const std::shared_ptr<Pending>& p) {
	if (!connected_) return false;
	const std::string echo = "nd" + std::to_string(++seq_);
	p->action = action;
	p->start = steady_clock::now();
	{
		std::lock_guard<std::mutex> lk(pendMu_);
		pending_[echo] = p;
	}
	json req{{"action", action}, {"params", params.is_null() ? json::object() : std::move(params)}, {"echo", echo}};
	const std::string text = req.dump(-1, ' ', false, json::error_handler_t::replace);
	bool sent = false;
	{
		std::lock_guard<std::mutex> lk(wsMu_);
		sent = ws_ && ws_->send_text(text);
	}
	if (!sent) {
		std::lock_guard<std::mutex> lk(pendMu_);
		pending_.erase(echo);
		return false;
	}
	logx::debug("→ " + action + " " + text.substr(0, 300));
	return true;
}

OneBot::Result OneBot::call(const std::string& action, json params, int timeout_ms) {
	auto p = std::make_shared<Pending>();
	if (!send_request(action, std::move(params), p)) {
		Result r;
		r.message = "未连接到 NapCat";
		return r;
	}
	std::unique_lock<std::mutex> lk(p->m);
	if (!p->cv.wait_for(lk, milliseconds(timeout_ms), [&] { return p->done; })) {
		Result r;
		r.message = "调用 " + action + " 超时";
		return r;
	}
	return p->r;
}

void OneBot::post(const std::string& action, json params) {
	auto p = std::make_shared<Pending>();
	p->fire = true;
	if (!send_request(action, std::move(params), p)) {
		logx::warn("调用 " + action + " 失败：未连接到 NapCat");
	}
}

void OneBot::handle_text(std::string&& text) {
	lastRecvMs_ = now_ms();
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object()) {
		logx::debug("忽略无法解析的数据: " + text.substr(0, 200));
		return;
	}
	if (j.contains("post_type")) {
		if (onEvent_) onEvent_(std::move(j));
		return;
	}
	auto it = j.find("echo");
	if (it == j.end() || !it->is_string()) return;
	std::shared_ptr<Pending> p;
	{
		std::lock_guard<std::mutex> lk(pendMu_);
		auto pi = pending_.find(it->get<std::string>());
		if (pi == pending_.end()) return;
		p = pi->second;
		pending_.erase(pi);
	}
	Result r;
	r.retcode = j.value("retcode", -1);
	r.ok = j.value("status", std::string()) == "ok" || r.retcode == 0;
	if (j.contains("data")) r.data = std::move(j["data"]);
	if (j.contains("message") && j["message"].is_string()) r.message = j["message"].get<std::string>();
	if (r.message.empty() && j.contains("wording") && j["wording"].is_string()) r.message = j["wording"].get<std::string>();
	logx::debug("← " + p->action + (r.ok ? " ok" : " failed: " + r.message));
	if (p->fire) {
		if (!r.ok) logx::warn("调用 " + p->action + " 失败: " + (r.message.empty() ? "retcode=" + std::to_string(r.retcode) : r.message));
		return;
	}
	{
		std::lock_guard<std::mutex> lk(p->m);
		p->r = std::move(r);
		p->done = true;
	}
	p->cv.notify_all();
}

void OneBot::fail_all_pending(const std::string& why) {
	std::vector<std::shared_ptr<Pending>> all;
	{
		std::lock_guard<std::mutex> lk(pendMu_);
		for (auto& [k, v] : pending_) all.push_back(v);
		pending_.clear();
	}
	for (auto& p : all) {
		if (p->fire) continue;
		{
			std::lock_guard<std::mutex> lk(p->m);
			p->r.ok = false;
			p->r.message = why;
			p->done = true;
		}
		p->cv.notify_all();
	}
}

void OneBot::sweep_pending() {
	auto limit = steady_clock::now() - seconds(180);
	std::lock_guard<std::mutex> lk(pendMu_);
	for (auto it = pending_.begin(); it != pending_.end();) {
		if (it->second->fire && it->second->start < limit) it = pending_.erase(it);
		else ++it;
	}
}

void OneBot::manager_loop() {
	int backoffMs = 1000;
	int failures = 0;
	while (!stopping_) {
		auto c = std::make_unique<WsClient>();
		c->set_on_message([this](std::string&& s) { handle_text(std::move(s)); });
		c->set_on_close([this](const std::string& reason) {
			logx::warn("与 NapCat 的连接断开: " + reason);
			dropped_ = true;
			stateCv_.notify_all();
		});
		std::vector<std::pair<std::string, std::string>> headers;
		if (!token_.empty()) headers.emplace_back("Authorization", "Bearer " + token_);
		std::string err;
		dropped_ = false;
		if (!c->connect(url_, headers, 5000, err)) {
			if (failures == 0 || failures % 12 == 0) logx::warn("连接 NapCat（" + url_ + "）失败: " + err + "，稍后重试");
			++failures;
			std::unique_lock<std::mutex> lk(stateMu_);
			stateCv_.wait_for(lk, milliseconds(backoffMs), [this] { return stopping_.load(); });
			backoffMs = std::min(backoffMs * 2, 15000);
			continue;
		}
		failures = 0;
		backoffMs = 1000;
		{
			std::lock_guard<std::mutex> lk(wsMu_);
			ws_ = std::move(c);
		}
		lastRecvMs_ = now_ms();
		{
			std::lock_guard<std::mutex> lk(stateMu_);
			connected_ = true;
		}
		stateCv_.notify_all();
		logx::info("已连接 NapCat: " + url_);
		if (onState_) onState_(true);

		bool probing = false;
		while (!stopping_ && !dropped_) {
			{
				std::unique_lock<std::mutex> lk(stateMu_);
				stateCv_.wait_for(lk, seconds(5), [this] { return stopping_.load() || dropped_.load(); });
			}
			if (stopping_ || dropped_) break;
			sweep_pending();
			// Liveness probe when the line has been quiet for a while (heartbeats may be disabled).
			if (!probing && now_ms() - lastRecvMs_.load() > 60000) {
				probing = true;
				auto r = call("get_status", json::object(), 15000);
				probing = false;
				if (!r.ok && !dropped_ && !stopping_) {
					logx::warn("NapCat 长时间无响应，重新连接");
					break;
				}
			}
		}
		{
			std::lock_guard<std::mutex> lk(stateMu_);
			connected_ = false;
		}
		std::unique_ptr<WsClient> old;
		{
			std::lock_guard<std::mutex> lk(wsMu_);
			old = std::move(ws_);
		}
		if (old) old->close();
		fail_all_pending("与 NapCat 的连接已断开");
		if (onState_) onState_(false);
		if (!stopping_) {
			std::unique_lock<std::mutex> lk(stateMu_);
			stateCv_.wait_for(lk, milliseconds(1000), [this] { return stopping_.load(); });
		}
	}
}
