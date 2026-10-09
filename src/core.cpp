// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#include "core.hpp"

#include "log.hpp"
#include "platform.hpp"

template <typename T>
static bool resolve(void* h, const char* name, T& out, bool required, std::string& err) {
	out = reinterpret_cast<T>(plat::lib_sym(h, name));
	if (!out) {
		if (required) {
			err = std::string("核心缺少导出函数 ") + name + "，这可能不是 Dice! 核心，或版本过旧";
			return false;
		}
		logx::warn(std::string("核心没有导出 ") + name + "，相关事件将被忽略");
	}
	return true;
}

bool DiceCore::load(const std::filesystem::path& p, std::string& err) {
#ifdef _WIN32
	int bits = plat::pe_bits(p);
	if (bits > 0 && bits != plat::self_bits()) {
		err = "核心是 " + std::to_string(bits) + " 位的，当前 dice-host 是 " + std::to_string(plat::self_bits()) +
			" 位，请使用 " + std::to_string(bits) + " 位版本的 dice-host（发布包里的 win-x86 / win-x64 目录）";
		return false;
	}
#endif
	handle = plat::lib_open(p, err);
	if (!handle) return false;
	return resolve(handle, "eventStartUp", startUp, true, err) &&
		resolve(handle, "eventEnable", enable, true, err) &&
		resolve(handle, "eventExit", exit, true, err) &&
		resolve(handle, "eventPrivateMsg", privateMsg, true, err) &&
		resolve(handle, "eventGroupMsg", groupMsg, true, err) &&
		resolve(handle, "eventGroupMemberAdd", memberAdd, false, err) &&
		resolve(handle, "eventGroupMemberKicked", memberKicked, false, err) &&
		resolve(handle, "eventGroupBan", groupBan, false, err) &&
		resolve(handle, "eventGroupInvited", groupInvited, false, err) &&
		resolve(handle, "eventFriendRequest", friendRequest, false, err) &&
		resolve(handle, "eventFriendAdd", friendAdded, false, err) &&
		resolve(handle, "eventExtra", extra, false, err);
}
