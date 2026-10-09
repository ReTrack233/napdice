// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#include "driver.hpp"

#include <ctime>
#include <type_traits>

#include "DDAPI.h"
#include "log.hpp"
#include "platform.hpp"

#ifndef NAPDICE_VERSION
#define NAPDICE_VERSION "dev"
#endif

using nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono;

static Driver* g_drv = nullptr;
Driver* driver_instance() { return g_drv; }

// ---------------------------------------------------------------- json helpers
static long long jll(const json& j, const char* k, long long def = 0) {
	auto it = j.find(k);
	if (it == j.end()) return def;
	if (it->is_number_integer()) return it->get<long long>();
	if (it->is_number()) return static_cast<long long>(it->get<double>());
	if (it->is_string()) {
		try {
			return std::stoll(it->get<std::string>());
		}
		catch (...) {
		}
	}
	return def;
}
static std::string jstr(const json& j, const char* k) {
	auto it = j.find(k);
	if (it != j.end() && it->is_string()) return it->get<std::string>();
	return {};
}
static int role_of(const std::string& r) {
	if (r == "owner") return 3;
	if (r == "admin") return 2;
	if (r == "member") return 1;
	return 0;
}
static std::string id_str(long long v) { return std::to_string(v); }

static std::string format_duration(long long sec) {
	if (sec <= 0) return "";
	std::string s;
	long long d = sec / 86400, h = sec % 86400 / 3600, m = sec % 3600 / 60, x = sec % 60;
	if (d) s += std::to_string(d) + "天";
	if (h) s += std::to_string(h) + "小时";
	if (m) s += std::to_string(m) + "分钟";
	if (x) s += std::to_string(x) + "秒";
	return s;
}

// ---------------------------------------------------------------- KeyedPool
KeyedPool::KeyedPool(int lanes) {
	for (int i = 0; i < lanes; ++i) {
		auto lane = std::make_unique<Lane>();
		Lane* l = lane.get();
		l->t = std::thread([this, l] {
			for (;;) {
				std::function<void()> fn;
				{
					std::unique_lock<std::mutex> lk(l->m);
					l->cv.wait(lk, [&] { return stopping_.load() || !l->q.empty(); });
					if (stopping_) return;
					fn = std::move(l->q.front());
					l->q.pop_front();
				}
				try {
					fn();
				}
				catch (const std::exception& e) {
					logx::error(std::string("处理事件时出现异常: ") + e.what());
				}
				catch (...) {
					logx::error("处理事件时出现未知异常");
				}
			}
		});
		lanes_.push_back(std::move(lane));
	}
}

KeyedPool::~KeyedPool() { stop(); }

void KeyedPool::post(long long key, std::function<void()> fn) {
	if (stopping_ || lanes_.empty()) return;
	auto idx = static_cast<size_t>(static_cast<unsigned long long>(key) % lanes_.size());
	Lane& l = *lanes_[idx];
	{
		std::lock_guard<std::mutex> lk(l.m);
		if (l.q.size() >= 2000) {
			l.q.pop_front();
			logx::warn("事件积压过多，丢弃最早的一条（骰娘处理不过来了？）");
		}
		l.q.push_back(std::move(fn));
	}
	l.cv.notify_one();
}

void KeyedPool::stop() {
	if (stopping_.exchange(true)) return;
	for (auto& l : lanes_) {
		{
			std::lock_guard<std::mutex> lk(l->m);
			l->q.clear();
		}
		l->cv.notify_all();
	}
	for (auto& l : lanes_)
		if (l->t.joinable()) l->t.join();
}

// ---------------------------------------------------------------- Driver
Driver::Driver(const Config& cfg, OneBot& ob) : cfg_(cfg), ob_(ob) { g_drv = this; }

Driver::~Driver() {
	if (pool_) pool_->stop();
	if (g_drv == this) g_drv = nullptr;
}

void Driver::set_request(Request r) {
	Request cur = request_.load();
	// Stop wins over restart
	if (cur == Request::Stop) return;
	request_ = r;
}

std::string Driver::self_nick() {
	std::lock_guard<std::mutex> lk(mu_);
	return selfNick_;
}

cq::MediaDirs Driver::media_dirs() const {
	return {cfg_.root, cfg_.root / ("Dice" + std::to_string(selfId_.load())), cfg_.media_base64};
}

void Driver::on_state(bool connected) {
	if (!connected) return;
	auto r = ob_.call("get_login_info", json::object(), cfg_.rpc_timeout_ms);
	if (!r.ok) {
		logx::error("获取登录账号失败: " + r.message);
		return;
	}
	long long id = jll(r.data, "user_id");
	if (!id) {
		logx::error("NapCat 返回的登录账号无效");
		return;
	}
	long long old = selfId_.load();
	if (old && old != id) {
		logx::warn("NapCat 登录的账号从 " + std::to_string(old) + " 变成了 " + std::to_string(id) + "，重新加载骰娘");
		set_request(Request::Restart);
		return;
	}
	selfId_ = id;
	{
		std::lock_guard<std::mutex> lk(mu_);
		selfNick_ = jstr(r.data, "nickname");
		// Reconnected: everything we cached may be stale.
		friendsAt_ = groupsAt_ = friendsRetry_ = groupsRetry_ = clock::time_point{};
		for (auto& [gid, gm] : members_) gm.full = false;
	}
	if (driverVer_.empty()) {
		std::string onebot = "OneBot 11";
		auto v = ob_.call("get_version_info", json::object(), cfg_.rpc_timeout_ms);
		if (v.ok) onebot = jstr(v.data, "app_name") + " " + jstr(v.data, "app_version");
		driverVer_ = std::string("NapDice dice-host ") + NAPDICE_VERSION + " (" + onebot + ")";
	}
	ensure_friends();
	ensure_groups();
	{
		std::lock_guard<std::mutex> lk(loginMu_);
		loginKnown_ = true;
	}
	loginCv_.notify_all();
	logx::info("骰娘账号: " + self_nick() + "(" + std::to_string(id) + ")，好友 " + std::to_string(friend_list().size()) +
		" 个，群 " + std::to_string(group_list().size()) + " 个");
}

bool Driver::wait_login(const std::atomic<bool>& stop) {
	auto lastHint = clock::now();
	std::unique_lock<std::mutex> lk(loginMu_);
	while (!loginKnown_) {
		if (stop.load()) return false;
		loginCv_.wait_for(lk, milliseconds(200));
		if (clock::now() - lastHint > seconds(30)) {
			lastHint = clock::now();
			logx::info("仍在等待连接 NapCat（" + cfg_.ws_url + "）……");
		}
	}
	return true;
}

void Driver::wait_for_link() {
	if (!ob_.connected()) ob_.wait_connected(10000);
}

// ---------------------------------------------------------------- caches
bool Driver::ensure_friends() {
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (fresh(friendsAt_, cfg_.list_cache_sec)) return true;
		if (clock::now() < friendsRetry_) return friendsAt_ != clock::time_point{};
	}
	std::lock_guard<std::mutex> rk(friendsRefreshMu_);
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (fresh(friendsAt_, cfg_.list_cache_sec)) return true;
	}
	auto r = ob_.call("get_friend_list", json::object(), cfg_.rpc_timeout_ms * 2);
	std::lock_guard<std::mutex> lk(mu_);
	if (!r.ok || !r.data.is_array()) {
		friendsRetry_ = clock::now() + seconds(15);
		logx::warn("获取好友列表失败: " + r.message);
		return friendsAt_ != clock::time_point{};
	}
	friends_.clear();
	for (const auto& f : r.data) {
		long long uid = jll(f, "user_id");
		if (!uid) continue;
		friends_.insert(uid);
		std::string nick = jstr(f, "nickname");
		if (!nick.empty()) friendNick_[uid] = nick;
	}
	friendsAt_ = clock::now();
	return true;
}

bool Driver::ensure_groups() {
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (fresh(groupsAt_, cfg_.list_cache_sec)) return true;
		if (clock::now() < groupsRetry_) return groupsAt_ != clock::time_point{};
	}
	std::lock_guard<std::mutex> rk(groupsRefreshMu_);
	{
		std::lock_guard<std::mutex> lk(mu_);
		if (fresh(groupsAt_, cfg_.list_cache_sec)) return true;
	}
	auto r = ob_.call("get_group_list", json::object(), cfg_.rpc_timeout_ms * 2);
	std::lock_guard<std::mutex> lk(mu_);
	if (!r.ok || !r.data.is_array()) {
		groupsRetry_ = clock::now() + seconds(15);
		logx::warn("获取群列表失败: " + r.message);
		return groupsAt_ != clock::time_point{};
	}
	std::map<long long, GroupInfo> fresh_;
	auto now = clock::now();
	for (const auto& g : r.data) {
		long long gid = jll(g, "group_id");
		if (!gid) continue;
		GroupInfo& gi = fresh_[gid];
		gi.name = jstr(g, "group_name");
		gi.cur = static_cast<unsigned>(jll(g, "member_count"));
		gi.max = static_cast<unsigned>(jll(g, "max_member_count"));
		gi.at = now;
	}
	groups_ = std::move(fresh_);
	groupsAt_ = now;
	return true;
}

void Driver::store_member(long long gid, const json& j) {
	long long uid = jll(j, "user_id");
	if (!uid) return;
	Member& m = members_[gid].m[uid];
	if (j.contains("card")) m.card = jstr(j, "card");
	if (j.contains("nickname")) m.nick = jstr(j, "nickname");
	if (j.contains("role")) m.role = role_of(jstr(j, "role"));
	long long last = jll(j, "last_sent_time", 0);
	if (last > 0) m.lastSent = last;
	m.at = clock::now();
}

bool Driver::ensure_members(long long gid) {
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto it = members_.find(gid);
		if (it != members_.end() && it->second.full && fresh(it->second.fullAt, cfg_.member_cache_sec)) return true;
	}
	std::lock_guard<std::mutex> rk(membersRefreshMu_);
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto it = members_.find(gid);
		if (it != members_.end() && it->second.full && fresh(it->second.fullAt, cfg_.member_cache_sec)) return true;
	}
	auto r = ob_.call("get_group_member_list", json{{"group_id", id_str(gid)}, {"no_cache", false}}, cfg_.rpc_timeout_ms * 3);
	std::lock_guard<std::mutex> lk(mu_);
	if (!r.ok || !r.data.is_array()) {
		logx::warn("获取群 " + std::to_string(gid) + " 的成员列表失败: " + r.message);
		auto it = members_.find(gid);
		return it != members_.end() && it->second.full;  // stale but usable
	}
	GroupMembers& gm = members_[gid];
	gm.m.clear();
	for (const auto& j : r.data) store_member(gid, j);
	gm.full = true;
	gm.fullAt = clock::now();
	return true;
}

std::optional<Driver::Member> Driver::member(long long gid, long long uid, bool allowFetch) {
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto it = members_.find(gid);
		if (it != members_.end()) {
			auto mi = it->second.m.find(uid);
			if (mi != it->second.m.end() && fresh(mi->second.at, cfg_.member_cache_sec)) return mi->second;
			if (it->second.full && fresh(it->second.fullAt, cfg_.member_cache_sec) && mi == it->second.m.end()) return std::nullopt;
		}
	}
	if (!allowFetch) return std::nullopt;
	auto r = ob_.call("get_group_member_info", json{{"group_id", id_str(gid)}, {"user_id", id_str(uid)}, {"no_cache", false}},
					  cfg_.rpc_timeout_ms);
	std::lock_guard<std::mutex> lk(mu_);
	if (r.ok && r.data.is_object() && jll(r.data, "user_id")) {
		store_member(gid, r.data);
		return members_[gid].m[uid];
	}
	// Fall back to stale data when NapCat is unavailable
	auto it = members_.find(gid);
	if (it != members_.end()) {
		auto mi = it->second.m.find(uid);
		if (mi != it->second.m.end()) return mi->second;
	}
	return std::nullopt;
}

std::string Driver::nick_of(long long uid) {
	if (uid == selfId_.load()) return self_nick();
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto f = friendNick_.find(uid);
		if (f != friendNick_.end() && !f->second.empty()) return f->second;
		auto s = strangerNick_.find(uid);
		if (s != strangerNick_.end() && fresh(s->second.second, cfg_.list_cache_sec)) return s->second.first;
	}
	auto r = ob_.call("get_stranger_info", json{{"user_id", id_str(uid)}}, cfg_.rpc_timeout_ms);
	std::lock_guard<std::mutex> lk(mu_);
	if (r.ok && r.data.is_object()) {
		std::string nick = jstr(r.data, "nickname");
		strangerNick_[uid] = {nick, clock::now()};
		return nick;
	}
	auto s = strangerNick_.find(uid);
	return s != strangerNick_.end() ? s->second.first : std::string();
}

bool Driver::is_friend(long long uid, bool def) {
	if (!ensure_friends()) return def;
	std::lock_guard<std::mutex> lk(mu_);
	return friends_.count(uid) > 0;
}

std::set<long long> Driver::friend_list() {
	ensure_friends();
	std::lock_guard<std::mutex> lk(mu_);
	return friends_;
}

std::set<long long> Driver::group_list() {
	ensure_groups();
	std::lock_guard<std::mutex> lk(mu_);
	std::set<long long> s;
	for (const auto& [gid, gi] : groups_) s.insert(gid);
	return s;
}

std::optional<std::set<long long>> Driver::member_list(long long gid) {
	if (!ensure_members(gid)) return std::nullopt;
	std::lock_guard<std::mutex> lk(mu_);
	std::set<long long> s;
	for (const auto& [uid, m] : members_[gid].m) s.insert(uid);
	return s;
}

std::optional<std::set<long long>> Driver::admin_list(long long gid) {
	if (!ensure_members(gid)) return std::nullopt;
	std::lock_guard<std::mutex> lk(mu_);
	std::set<long long> s;
	for (const auto& [uid, m] : members_[gid].m)
		if (m.role >= 2) s.insert(uid);
	return s;
}

int Driver::group_auth(long long gid, long long uid) {
	auto m = member(gid, uid, true);
	return m ? m->role : 0;
}

std::optional<bool> Driver::is_member(long long gid, long long uid) {
	if (uid == selfId_.load()) {
		if (!ensure_groups()) return std::nullopt;
		std::lock_guard<std::mutex> lk(mu_);
		return groups_.count(gid) > 0;
	}
	if (auto m = member(gid, uid, false)) return true;
	if (!ensure_members(gid)) return std::nullopt;
	std::lock_guard<std::mutex> lk(mu_);
	return members_[gid].m.count(uid) > 0;
}

std::string Driver::group_name(long long gid) {
	ensure_groups();
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto it = groups_.find(gid);
		if (it != groups_.end() && !it->second.name.empty()) return it->second.name;
	}
	auto r = ob_.call("get_group_info", json{{"group_id", id_str(gid)}}, cfg_.rpc_timeout_ms);
	if (!r.ok || !r.data.is_object()) return {};
	return jstr(r.data, "group_name");
}

std::pair<unsigned, unsigned> Driver::group_size(long long gid) {
	ensure_groups();
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto it = groups_.find(gid);
		if (it != groups_.end() && it->second.cur) return {it->second.cur, it->second.max};
	}
	auto r = ob_.call("get_group_info", json{{"group_id", id_str(gid)}}, cfg_.rpc_timeout_ms);
	if (!r.ok || !r.data.is_object()) return {0, 0};
	return {static_cast<unsigned>(jll(r.data, "member_count")), static_cast<unsigned>(jll(r.data, "max_member_count"))};
}

std::string Driver::group_nick(long long gid, long long uid) {
	if (auto m = member(gid, uid, true)) {
		if (!m->card.empty()) return m->card;
		if (!m->nick.empty()) return m->nick;
	}
	return nick_of(uid);
}

long long Driver::group_last_msg(long long gid, long long uid) {
	auto m = member(gid, uid, false);
	if (!m && ensure_members(gid)) m = member(gid, uid, false);
	return m && m->lastSent > 0 ? m->lastSent : -1;
}

// ---------------------------------------------------------------- actions
void Driver::send_private(long long uid, const std::string& msg) {
	wait_for_link();
	json p{{"user_id", id_str(uid)}, {"message", cq::rewrite_outgoing(msg, media_dirs())}};
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto t = tempSession_.find(uid);
		if (t != tempSession_.end() && !friends_.count(uid)) p["group_id"] = id_str(t->second);
	}
	ob_.post("send_private_msg", std::move(p));
}

void Driver::send_group(long long gid, const std::string& msg) {
	wait_for_link();
	ob_.post("send_group_msg", json{{"group_id", id_str(gid)}, {"message", cq::rewrite_outgoing(msg, media_dirs())}});
}

void Driver::answer_friend(long long uid, int respon) {
	std::string flag;
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto it = friendReq_.find(uid);
		if (it != friendReq_.end()) {
			flag = it->second.first;
			friendReq_.erase(it);
		}
	}
	if (flag.empty()) {
		// The core answers a group-invite hook through AnswerFriendRequest(inviter); route it.
		long long gid = 0;
		{
			std::lock_guard<std::mutex> lk(mu_);
			for (const auto& [g, inv] : groupInvite_)
				if (inv.second == uid) gid = g;
		}
		if (gid) {
			answer_invite(gid, respon);
			return;
		}
		logx::warn("没有找到 " + std::to_string(uid) + " 的好友申请，无法处理");
		return;
	}
	if (respon != 1 && respon != 2) return;  // 3 = ignore
	ob_.post("set_friend_add_request", json{{"flag", flag}, {"approve", respon == 1}});
}

void Driver::answer_invite(long long gid, int respon) {
	std::string flag;
	{
		std::lock_guard<std::mutex> lk(mu_);
		auto it = groupInvite_.find(gid);
		if (it != groupInvite_.end()) {
			flag = it->second.first;
			groupInvite_.erase(it);
		}
	}
	if (flag.empty()) {
		logx::warn("没有找到群 " + std::to_string(gid) + " 的入群邀请，无法处理");
		return;
	}
	if (respon != 1 && respon != 2) return;
	ob_.post("set_group_add_request", json{{"flag", flag}, {"sub_type", "invite"}, {"approve", respon == 1}, {"reason", ""}});
}

bool Driver::upload_file(long long gid, long long uid, const std::string& path) {
	fs::path p = plat::u8path(path);
	std::error_code ec;
	if (p.is_relative()) p = cfg_.root / p;
	std::string abs = plat::path_u8(fs::absolute(p, ec));
	std::string name = plat::path_u8(p.filename());
	if (cfg_.media_base64) {
		abs = cq::file_to_base64_uri(p);
		if (abs.empty()) {
			logx::warn("读取文件失败: " + plat::path_u8(p));
			return false;
		}
	}
	auto r = gid ? ob_.call("upload_group_file", json{{"group_id", id_str(gid)}, {"file", abs}, {"name", name}}, 120000)
				 : ob_.call("upload_private_file", json{{"user_id", id_str(uid)}, {"file", abs}, {"name", name}}, 120000);
	if (!r.ok) logx::warn("上传文件失败: " + r.message);
	return r.ok;
}

std::string Driver::extra(const std::string& data) {
	logx::debug("askExtra: " + data.substr(0, 500));
	json req = json::parse(data, nullptr, false);
	auto fail = [](const std::string& m) { return json{{"status", "failed"}, {"retcode", -1}, {"message", m}}.dump(); };
	if (req.is_discarded() || !req.is_object()) return fail("askExtra 需要传入 {action=..., params={...}}");
	std::string action = jstr(req, "action");
	if (action.empty()) return fail("缺少 action");
	if (action == "getGender") {
		// Asked by the core itself for the {gender} variable; the answer is the bare value.
		long long uid = jll(req, "uid");
		auto r = uid ? ob_.call("get_stranger_info", json{{"user_id", id_str(uid)}}, cfg_.rpc_timeout_ms) : OneBot::Result{};
		std::string sex = r.ok ? jstr(r.data, "sex") : "";
		return json(sex == "male" ? "男" : sex == "female" ? "女" : "未知").dump();
	}
	if (action == "napdice_info") {
		return json{{"status", "ok"},
					{"retcode", 0},
					{"data", {{"driver", driverVer_}, {"self_id", selfId_.load()}, {"connected", ob_.connected()}}}}
			.dump(-1, ' ', false, json::error_handler_t::replace);
	}
	// Either {action, params={...}} or a flat {action, key=value...}
	json params = json::object();
	if (req.contains("params") && req["params"].is_object()) {
		params = req["params"];
	}
	else {
		for (auto it = req.begin(); it != req.end(); ++it) {
			if (it.key() != "action" && it.key() != "async" && it.key() != "timeout") params[it.key()] = it.value();
		}
	}
	const json& asyncFlag = req.contains("async") ? req["async"] : json();
	const bool async = asyncFlag.is_boolean() ? asyncFlag.get<bool>() : (asyncFlag.is_number() && asyncFlag.get<double>() != 0);
	if (async) {
		ob_.post(action, std::move(params));
		return json{{"status", "async"}, {"retcode", 0}}.dump();
	}
	int timeout = static_cast<int>(jll(req, "timeout", cfg_.rpc_timeout_ms));
	auto r = ob_.call(action, std::move(params), timeout);
	json out{{"status", r.ok ? "ok" : "failed"}, {"retcode", r.retcode}, {"data", r.data}};
	if (!r.message.empty()) out["message"] = r.message;
	return out.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ---------------------------------------------------------------- events
void Driver::dispatch(long long key, std::function<void()> fn) {
	if (!ready_ || !pool_) {
		logx::debug("骰娘尚未就绪，忽略事件");
		return;
	}
	pool_->post(key, std::move(fn));
}

void Driver::on_event(json&& ev) {
	const std::string pt = jstr(ev, "post_type");
	if (pt == "message") handle_message(ev);
	else if (pt == "notice") handle_notice(ev);
	else if (pt == "request") handle_request(ev);
	// meta_event / message_sent: nothing to do
}

void Driver::handle_message(const json& ev) {
	const std::string type = jstr(ev, "message_type");
	const long long uid = jll(ev, "user_id");
	const int msgId = static_cast<int>(jll(ev, "message_id"));
	std::string raw = jstr(ev, "raw_message");
	if (!uid || raw.empty()) return;
	const json sender = ev.contains("sender") && ev["sender"].is_object() ? ev["sender"] : json::object();
	if (type == "group") {
		const long long gid = jll(ev, "group_id");
		if (!gid) return;
		{
			std::lock_guard<std::mutex> lk(mu_);
			Member& m = members_[gid].m[uid];
			if (sender.contains("card")) m.card = jstr(sender, "card");
			if (sender.contains("nickname")) m.nick = jstr(sender, "nickname");
			if (sender.contains("role")) m.role = role_of(jstr(sender, "role"));
			m.lastSent = jll(ev, "time", static_cast<long long>(std::time(nullptr)));
			m.at = clock::now();
		}
		std::string msg = cq::normalize_incoming(raw, cfg_.strip_reply);
		logx::debug("[群" + std::to_string(gid) + "] " + std::to_string(uid) + ": " + msg);
		dispatch(gid, [this, msgId, gid, uid, msg] { core_->groupMsg(msgId, gid, uid, msg.c_str()); });
	}
	else if (type == "private") {
		if (jstr(ev, "sub_type") == "group") {
			long long gid = jll(ev, "group_id");
			if (!gid && sender.contains("group_id")) gid = jll(sender, "group_id");
			if (gid) {
				std::lock_guard<std::mutex> lk(mu_);
				tempSession_[uid] = gid;
			}
		}
		std::string nick = jstr(sender, "nickname");
		if (!nick.empty()) {
			std::lock_guard<std::mutex> lk(mu_);
			strangerNick_[uid] = {nick, clock::now()};
		}
		std::string msg = cq::normalize_incoming(raw, cfg_.strip_reply);
		logx::debug("[私聊] " + std::to_string(uid) + ": " + msg);
		dispatch(-uid, [this, msgId, uid, msg] { core_->privateMsg(msgId, uid, msg.c_str()); });
	}
}

void Driver::handle_notice(const json& ev) {
	const std::string nt = jstr(ev, "notice_type");
	const std::string st = jstr(ev, "sub_type");
	const long long gid = jll(ev, "group_id");
	const long long uid = jll(ev, "user_id");
	const long long op = jll(ev, "operator_id");
	const long long self = selfId_.load();

	if (nt == "group_increase") {
		{
			std::lock_guard<std::mutex> lk(mu_);
			if (uid == self) groupsAt_ = clock::time_point{};
			members_[gid].full = false;
		}
		if (core_ && core_->memberAdd)
			dispatch(gid, [this, gid, uid, op] { core_->memberAdd(gid, uid, op); });
	}
	else if (nt == "group_decrease") {
		{
			std::lock_guard<std::mutex> lk(mu_);
			if (uid == self || st == "kick_me") {
				groups_.erase(gid);
				members_.erase(gid);
			}
			else {
				members_[gid].m.erase(uid);
			}
		}
		if (core_ && core_->memberKicked && (st == "kick" || st == "kick_me")) {
			long long kicked = st == "kick_me" ? self : uid;
			dispatch(gid, [this, gid, kicked, op] { core_->memberKicked(gid, kicked, op); });
		}
	}
	else if (nt == "group_ban") {
		if (!uid) return;  // whole-group ban
		if (core_ && core_->groupBan) {
			std::string dur = st == "lift_ban" ? std::string() : format_duration(jll(ev, "duration"));
			dispatch(gid, [this, gid, uid, op, dur] { core_->groupBan(gid, uid, op, dur.c_str()); });
		}
	}
	else if (nt == "group_admin") {
		std::lock_guard<std::mutex> lk(mu_);
		auto& m = members_[gid].m[uid];
		m.role = st == "set" ? 2 : 1;
		m.at = clock::now();
	}
	else if (nt == "group_card") {
		std::lock_guard<std::mutex> lk(mu_);
		auto& m = members_[gid].m[uid];
		m.card = jstr(ev, "card_new");
	}
	else if (nt == "friend_add") {
		{
			std::lock_guard<std::mutex> lk(mu_);
			friends_.insert(uid);
		}
		if (core_ && core_->friendAdded)
			dispatch(-uid, [this, uid] { core_->friendAdded(uid); });
	}
	else if (nt == "notify" && st == "poke") {
		if (!cfg_.forward_poke || uid == self || !core_ || !core_->extra) return;
		json e{{"Event", "Poke"}, {"uid", uid}, {"target", jll(ev, "target_id")}, {"time", jll(ev, "time")}};
		if (gid) e["gid"] = gid;
		std::string s = e.dump();
		dispatch(gid ? gid : -uid, [this, s] { core_->extra(s.c_str()); });
	}
	else if (cfg_.forward_notice && core_ && core_->extra) {
		json e{{"Event", "OneBotNotice"}, {"notice_type", nt}, {"sub_type", st}, {"raw", ev.dump(-1, ' ', false, json::error_handler_t::replace)}};
		if (uid) e["uid"] = uid;
		if (gid) e["gid"] = gid;
		std::string s = e.dump(-1, ' ', false, json::error_handler_t::replace);
		dispatch(gid ? gid : -uid, [this, s] { core_->extra(s.c_str()); });
	}
}

void Driver::handle_request(const json& ev) {
	const std::string rt = jstr(ev, "request_type");
	const std::string st = jstr(ev, "sub_type");
	const long long uid = jll(ev, "user_id");
	const long long gid = jll(ev, "group_id");
	const std::string flag = jstr(ev, "flag");
	const std::string comment = jstr(ev, "comment");
	if (rt == "friend") {
		{
			std::lock_guard<std::mutex> lk(mu_);
			friendReq_[uid] = {flag, clock::now()};
		}
		logx::info("收到好友申请: " + std::to_string(uid) + " " + comment);
		if (core_ && core_->friendRequest)
			dispatch(-uid, [this, uid, comment] { core_->friendRequest(uid, comment.c_str()); });
	}
	else if (rt == "group" && st == "invite") {
		{
			std::lock_guard<std::mutex> lk(mu_);
			groupInvite_[gid] = {flag, uid};
		}
		logx::info("收到入群邀请: 群 " + std::to_string(gid) + "，邀请人 " + std::to_string(uid));
		if (core_ && core_->groupInvited)
			dispatch(gid, [this, gid, uid] { core_->groupInvited(gid, uid); });
	}
	else if (cfg_.forward_notice && core_ && core_->extra) {
		json e{{"Event", "OneBotRequest"}, {"request_type", rt}, {"sub_type", st}, {"uid", uid}, {"flag", flag}, {"comment", comment}};
		if (gid) e["gid"] = gid;
		std::string s = e.dump(-1, ' ', false, json::error_handler_t::replace);
		dispatch(gid ? gid : -uid, [this, s] { core_->extra(s.c_str()); });
	}
}

// ================================================================ DD API
// Signatures must match Dice's QQAPI/DDAPI.cpp exactly; they are checked with static_assert below.
#define DDAPI(Name, ReturnType, ...) using Name##_TYPE = ReturnType (*)(__VA_ARGS__)
DDAPI(_DriverVer, const char*);
DDAPI(GetRootDir, const std::string&);
DDAPI(Reload, bool);
DDAPI(Remake, bool);
DDAPI(Killme, void);
DDAPI(DebugLog, void, const std::string&);
DDAPI(DebugMsg, void, long long, const std::string&);
DDAPI(IsDiceMaid, bool, long long);
DDAPI(GetDiceSisters, const std::set<long long>&);
DDAPI(GetNick, const std::string&, long long, long long);
DDAPI(SendPrivateMsg, void, long long, long long, const std::string&);
DDAPI(SendGroupMsg, void, long long, long long, const std::string&);
DDAPI(SendChannelMsg, void, long long, long long, long long, const std::string&);
DDAPI(SendDiscussMsg, void, long long, long long, const std::string&);
DDAPI(IsFriend, bool, long long, long long, bool);
DDAPI(GetFriendQQList, const std::set<long long>&, long long);
DDAPI(GetGroupIDList, const std::set<long long>&, long long);
DDAPI(GetGroupMemberList, const std::set<long long>&, long long, long long);
DDAPI(GetGroupAdminList, const std::set<long long>&, long long, long long);
DDAPI(GetGroupAuth, int, long long, long long, long long);
DDAPI(IsGroupAdmin, bool, long long, long long, long long, bool);
DDAPI(IsGroupOwner, bool, long long, long long, long long, bool);
DDAPI(IsGroupMember, bool, long long, long long, long long, bool);
DDAPI(AnswerFriendRequest, void, long long, long long, int, const std::string&);
DDAPI(AnswerGroupInvited, void, long long, long long, int);
DDAPI(GetGroupSize, const GroupSize_t, long long, long long);
DDAPI(GetGroupName, const std::string&, long long, long long);
DDAPI(GetGroupNick, const std::string&, long long, long long, long long);
DDAPI(GetGroupLastMsg, long long, long long, long long, long long);
DDAPI(SetGroupKick, void, long long, long long, long long);
DDAPI(SetGroupBan, void, long long, long long, long long, int);
DDAPI(SetGroupAdmin, void, long long, long long, long long, bool);
DDAPI(SetGroupCard, void, long long, long long, long long, const std::string&);
DDAPI(SetGroupTitle, void, long long, long long, long long, const std::string&);
DDAPI(SetGroupWholeBan, void, long long, long long, int);
DDAPI(SetGroupLeave, void, long long, long long);
DDAPI(SetDiscussLeave, void, long long, long long);
DDAPI(UploadGroupFile, bool, long long, long long, const std::string&);
DDAPI(SendPrivateFile, bool, long long, long long, const std::string&);
DDAPI(GetExtra, bool, const std::string&, std::string&);
#undef DDAPI

// Every DD function swallows exceptions: nothing may unwind into the core.
#define GUARD_BEGIN try {
#define GUARD_END(fallback)                                                     \
	}                                                                           \
	catch (const std::exception& e) {                                           \
		logx::error(std::string("驱动接口 ") + __func__ + " 出错: " + e.what()); \
	}                                                                           \
	catch (...) {                                                               \
		logx::error(std::string("驱动接口 ") + __func__ + " 出现未知错误");     \
	}                                                                           \
	return fallback;

namespace dd {

// Filled in by Driver::start_core() before the core starts, read-only afterwards.
static std::string g_driverVer;
static std::string g_rootDir;

static const char* _DriverVer() { return g_driverVer.c_str(); }
static const std::string& GetRootDir() { return g_rootDir; }
static bool Reload() {
	logx::info("骰娘请求重载，即将重启核心");
	if (g_drv) g_drv->set_request(Driver::Request::Restart);
	return true;
}
static bool Remake() { return Reload(); }
static void Killme() {
	logx::info("骰娘请求退出");
	if (g_drv) g_drv->set_request(Driver::Request::Stop);
}
static void DebugLog(const std::string& s) { logx::info("[Dice] " + s); }
static void DebugMsg(long long, const std::string& s) { logx::info("[Dice!] " + s); }
static bool IsDiceMaid(long long id) { return g_drv && id == g_drv->self_id(); }
static const std::set<long long>& GetDiceSisters() {
	thread_local std::set<long long> s;
	s.clear();
	if (g_drv) s.insert(g_drv->self_id());
	return s;
}
static const std::string& GetNick(long long, long long uid) {
	thread_local std::string s;
	GUARD_BEGIN
	s = g_drv->nick_of(uid);
	return s;
	GUARD_END((s.clear(), s))
}
static void SendPrivateMsg(long long, long long uid, const std::string& msg) {
	GUARD_BEGIN
	g_drv->send_private(uid, msg);
	return;
	GUARD_END()
}
static void SendGroupMsg(long long, long long gid, const std::string& msg) {
	GUARD_BEGIN
	g_drv->send_group(gid, msg);
	return;
	GUARD_END()
}
static void SendChannelMsg(long long, long long, long long, const std::string&) {
	logx::warn("NapCat 不支持 QQ 频道消息，已忽略");
}
static void SendDiscussMsg(long long, long long, const std::string&) { logx::warn("讨论组已停用，已忽略"); }
static bool IsFriend(long long, long long uid, bool def) {
	GUARD_BEGIN
	return g_drv->is_friend(uid, def);
	GUARD_END(def)
}
static const std::set<long long>& GetFriendQQList(long long) {
	thread_local std::set<long long> s;
	GUARD_BEGIN
	s = g_drv->friend_list();
	return s;
	GUARD_END((s.clear(), s))
}
static const std::set<long long>& GetGroupIDList(long long) {
	thread_local std::set<long long> s;
	GUARD_BEGIN
	s = g_drv->group_list();
	return s;
	GUARD_END((s.clear(), s))
}
static const std::set<long long>& GetGroupMemberList(long long, long long gid) {
	thread_local std::set<long long> s;
	GUARD_BEGIN
	s = g_drv->member_list(gid).value_or(std::set<long long>{});
	return s;
	GUARD_END((s.clear(), s))
}
static const std::set<long long>& GetGroupAdminList(long long, long long gid) {
	thread_local std::set<long long> s;
	GUARD_BEGIN
	s = g_drv->admin_list(gid).value_or(std::set<long long>{});
	return s;
	GUARD_END((s.clear(), s))
}
static int GetGroupAuth(long long, long long gid, long long uid) {
	GUARD_BEGIN
	return g_drv->group_auth(gid, uid);
	GUARD_END(0)
}
static bool IsGroupAdmin(long long, long long gid, long long uid, bool def) {
	GUARD_BEGIN
	int a = g_drv->group_auth(gid, uid);
	return a ? a >= 2 : def;
	GUARD_END(def)
}
static bool IsGroupOwner(long long, long long gid, long long uid, bool def) {
	GUARD_BEGIN
	int a = g_drv->group_auth(gid, uid);
	return a ? a == 3 : def;
	GUARD_END(def)
}
static bool IsGroupMember(long long, long long gid, long long uid, bool def) {
	GUARD_BEGIN
	return g_drv->is_member(gid, uid).value_or(def);
	GUARD_END(def)
}
static void AnswerFriendRequest(long long, long long uid, int respon, const std::string&) {
	GUARD_BEGIN
	g_drv->answer_friend(uid, respon);
	return;
	GUARD_END()
}
static void AnswerGroupInvited(long long, long long gid, int respon) {
	GUARD_BEGIN
	g_drv->answer_invite(gid, respon);
	return;
	GUARD_END()
}
static const GroupSize_t GetGroupSize(long long, long long gid) {
	GroupSize_t r;
	GUARD_BEGIN
	auto [cur, max] = g_drv->group_size(gid);
	r.currSize = cur;
	r.maxSize = max;
	return r;
	GUARD_END(r)
}
static const std::string& GetGroupName(long long, long long gid) {
	thread_local std::string s;
	GUARD_BEGIN
	s = g_drv->group_name(gid);
	return s;
	GUARD_END((s.clear(), s))
}
static const std::string& GetGroupNick(long long, long long gid, long long uid) {
	thread_local std::string s;
	GUARD_BEGIN
	s = g_drv->group_nick(gid, uid);
	return s;
	GUARD_END((s.clear(), s))
}
static long long GetGroupLastMsg(long long, long long gid, long long uid) {
	GUARD_BEGIN
	return g_drv->group_last_msg(gid, uid);
	GUARD_END(-1LL)
}
static void SetGroupKick(long long, long long gid, long long uid) {
	g_drv->ob().post("set_group_kick", json{{"group_id", id_str(gid)}, {"user_id", id_str(uid)}, {"reject_add_request", false}});
}
static void SetGroupBan(long long, long long gid, long long uid, int sec) {
	g_drv->ob().post("set_group_ban", json{{"group_id", id_str(gid)}, {"user_id", id_str(uid)}, {"duration", sec}});
}
static void SetGroupAdmin(long long, long long gid, long long uid, bool on) {
	g_drv->ob().post("set_group_admin", json{{"group_id", id_str(gid)}, {"user_id", id_str(uid)}, {"enable", on}});
}
static void SetGroupCard(long long, long long gid, long long uid, const std::string& card) {
	g_drv->ob().post("set_group_card", json{{"group_id", id_str(gid)}, {"user_id", id_str(uid)}, {"card", card}});
}
static void SetGroupTitle(long long, long long gid, long long uid, const std::string& title) {
	g_drv->ob().post("set_group_special_title",
					 json{{"group_id", id_str(gid)}, {"user_id", id_str(uid)}, {"special_title", title}});
}
static void SetGroupWholeBan(long long, long long gid, int sec) {
	g_drv->ob().post("set_group_whole_ban", json{{"group_id", id_str(gid)}, {"enable", sec != 0}});
}
static void SetGroupLeave(long long, long long gid) {
	logx::info("退出群 " + std::to_string(gid));
	g_drv->ob().post("set_group_leave", json{{"group_id", id_str(gid)}, {"is_dismiss", false}});
}
static void SetDiscussLeave(long long, long long) { logx::warn("讨论组已停用，已忽略"); }
static bool UploadGroupFile(long long, long long gid, const std::string& path) {
	GUARD_BEGIN
	return g_drv->upload_file(gid, 0, path);
	GUARD_END(false)
}
static bool SendPrivateFile(long long, long long uid, const std::string& path) {
	GUARD_BEGIN
	return g_drv->upload_file(0, uid, path);
	GUARD_END(false)
}
static bool GetExtra(const std::string& data, std::string& ret) {
	GUARD_BEGIN
	ret = g_drv->extra(data);
	return true;
	GUARD_END(false)
}

} // namespace dd

static api_list g_api;
static const api_list& get_api_list() { return g_api; }

static void build_api() {
	g_api.clear();
#define REG(Name)                                                                                  \
	static_assert(std::is_same_v<decltype(&dd::Name), Name##_TYPE>, "DD API signature mismatch: " #Name); \
	g_api[#Name] = reinterpret_cast<void*>(&dd::Name)
	REG(_DriverVer);
	REG(GetRootDir);
	REG(Reload);
	REG(Remake);
	REG(Killme);
	REG(DebugLog);
	REG(DebugMsg);
	REG(IsDiceMaid);
	REG(GetDiceSisters);
	REG(GetNick);
	REG(SendPrivateMsg);
	REG(SendGroupMsg);
	REG(SendChannelMsg);
	REG(SendDiscussMsg);
	REG(IsFriend);
	REG(GetFriendQQList);
	REG(GetGroupIDList);
	REG(GetGroupMemberList);
	REG(GetGroupAdminList);
	REG(GetGroupAuth);
	REG(IsGroupAdmin);
	REG(IsGroupOwner);
	REG(IsGroupMember);
	REG(AnswerFriendRequest);
	REG(AnswerGroupInvited);
	REG(GetGroupSize);
	REG(GetGroupName);
	REG(GetGroupNick);
	REG(GetGroupLastMsg);
	REG(SetGroupKick);
	REG(SetGroupBan);
	REG(SetGroupAdmin);
	REG(SetGroupCard);
	REG(SetGroupTitle);
	REG(SetGroupWholeBan);
	REG(SetGroupLeave);
	REG(SetDiscussLeave);
	REG(UploadGroupFile);
	REG(SendPrivateFile);
	REG(GetExtra);
#undef REG
}

bool Driver::start_core(DiceCore* core, std::string& err) {
	bool lossless = true;
	rootNative_ = plat::utf8_to_native(plat::path_u8(cfg_.root), lossless);
	if (!lossless) {
		err = "根目录路径里有当前系统代码页无法表示的字符，Dice! 核心无法使用它: " + plat::path_u8(cfg_.root) +
			"。请换一个只含中英文和数字的路径";
		return false;
	}
	core_ = core;
	dd::g_driverVer = driverVer_;
	dd::g_rootDir = rootNative_;
	build_api();
	pool_ = std::make_unique<KeyedPool>(cfg_.workers);
	const long long self = selfId_.load();
	logx::info("启动 Dice! 核心，数据目录: " + plat::path_u8(cfg_.root / ("Dice" + std::to_string(self))));
	core_->startUp(reinterpret_cast<void*>(&get_api_list), self);
	core_->enable();
	ready_ = true;
	logx::info("Dice! 核心已就绪");
	return true;
}

void Driver::shutdown_core() {
	ready_ = false;
	if (pool_) pool_->stop();
	if (core_) {
		logx::info("正在保存数据并关闭 Dice! 核心……");
		core_->exit();
		logx::info("Dice! 核心已关闭");
	}
}
