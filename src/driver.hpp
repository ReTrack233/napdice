// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
//
// The DiceDriver side of the Dice! core: implements the DD API (QQAPI/DDAPI.h) on top of OneBot 11
// and turns OneBot events into calls of the core's exported event functions.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "config.hpp"
#include "core.hpp"
#include "cq.hpp"
#include "nlohmann/json.hpp"
#include "onebot.hpp"

// Runs tasks on N lanes; tasks with the same key always run on the same lane, in order.
class KeyedPool {
public:
	explicit KeyedPool(int lanes);
	~KeyedPool();
	void post(long long key, std::function<void()> fn);
	void stop();

private:
	struct Lane {
		std::mutex m;
		std::condition_variable cv;
		std::deque<std::function<void()>> q;
		std::thread t;
	};
	std::vector<std::unique_ptr<Lane>> lanes_;
	std::atomic<bool> stopping_{false};
};

class Driver {
public:
	using clock = std::chrono::steady_clock;
	enum class Request { None, Stop, Restart };

	Driver(const Config& cfg, OneBot& ob);
	~Driver();

	// OneBot hooks
	void on_event(nlohmann::json&& ev);
	void on_state(bool connected);

	// Blocks until the bot account is known (or `stop` becomes true).
	bool wait_login(const std::atomic<bool>& stop);
	bool start_core(DiceCore* core, std::string& err);
	void shutdown_core();

	Request request() const { return request_.load(); }
	void set_request(Request r);
	long long self_id() const { return selfId_.load(); }

	// ---- used by the DD API implementation ----
	const Config& cfg() const { return cfg_; }
	OneBot& ob() { return ob_; }
	const std::string& driver_ver() const { return driverVer_; }
	const std::string& root_native() const { return rootNative_; }
	std::string self_nick();

	std::string nick_of(long long uid);
	bool is_friend(long long uid, bool def);
	std::set<long long> friend_list();
	std::set<long long> group_list();
	std::optional<std::set<long long>> member_list(long long gid);
	std::optional<std::set<long long>> admin_list(long long gid);
	int group_auth(long long gid, long long uid);  // 0 unknown/not member, 1 member, 2 admin, 3 owner
	std::optional<bool> is_member(long long gid, long long uid);
	std::string group_name(long long gid);
	std::pair<unsigned, unsigned> group_size(long long gid);
	std::string group_nick(long long gid, long long uid);
	long long group_last_msg(long long gid, long long uid);

	void send_private(long long uid, const std::string& msg);
	void send_group(long long gid, const std::string& msg);
	void answer_friend(long long uid, int respon);
	void answer_invite(long long gid, int respon);
	bool upload_file(long long gid, long long uid, const std::string& path);
	std::string extra(const std::string& data);

private:
	struct Member {
		std::string card, nick;
		int role = 0;
		long long lastSent = -1;
		clock::time_point at{};
	};
	struct GroupMembers {
		std::unordered_map<long long, Member> m;
		bool full = false;
		clock::time_point fullAt{};
	};
	struct GroupInfo {
		std::string name;
		unsigned cur = 0, max = 0;
		clock::time_point at{};
	};

	bool fresh(clock::time_point t, int sec) const { return t != clock::time_point{} && clock::now() - t < std::chrono::seconds(sec); }
	bool ensure_friends();
	bool ensure_groups();
	bool ensure_members(long long gid);
	std::optional<Member> member(long long gid, long long uid, bool allowFetch);
	void store_member(long long gid, const nlohmann::json& j);
	void dispatch(long long key, std::function<void()> fn);
	void handle_message(const nlohmann::json& ev);
	void handle_notice(const nlohmann::json& ev);
	void handle_request(const nlohmann::json& ev);
	void wait_for_link();
	cq::MediaDirs media_dirs() const;

	const Config& cfg_;
	OneBot& ob_;
	DiceCore* core_ = nullptr;
	std::unique_ptr<KeyedPool> pool_;
	std::atomic<bool> ready_{false};
	std::atomic<Request> request_{Request::None};
	std::atomic<long long> selfId_{0};
	std::string driverVer_;
	std::string rootNative_;

	std::mutex loginMu_;
	std::condition_variable loginCv_;
	bool loginKnown_ = false;

	std::mutex mu_;  // guards all caches below
	std::string selfNick_;
	std::set<long long> friends_;
	std::unordered_map<long long, std::string> friendNick_;
	clock::time_point friendsAt_{}, friendsRetry_{};
	std::map<long long, GroupInfo> groups_;
	clock::time_point groupsAt_{}, groupsRetry_{};
	std::unordered_map<long long, GroupMembers> members_;
	std::unordered_map<long long, std::pair<std::string, clock::time_point>> strangerNick_;
	std::unordered_map<long long, std::pair<std::string, clock::time_point>> friendReq_;           // uid -> flag
	std::unordered_map<long long, std::pair<std::string, long long>> groupInvite_;                 // gid -> flag, inviter
	std::unordered_map<long long, long long> tempSession_;                                         // uid -> gid
	std::mutex friendsRefreshMu_, groupsRefreshMu_, membersRefreshMu_;
};

Driver* driver_instance();
