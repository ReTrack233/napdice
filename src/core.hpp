// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
//
// Loads the Dice! core library and resolves its exported event functions (see Dice QQAPI/QQEvent.h).
#pragma once
#include <filesystem>
#include <string>

// 32-bit Windows builds of the core export __stdcall functions; everywhere else the default
// calling convention is used.
#if defined(_WIN32) && !defined(_WIN64)
#define DICE_EVCALL __stdcall
#else
#define DICE_EVCALL
#endif

struct DiceCore {
	using FnStartup = void(DICE_EVCALL*)(void* initApi, long long botQQ);
	using FnVoid = void(DICE_EVCALL*)();
	using FnPrivateMsg = int(DICE_EVCALL*)(int msgId, long long fromUID, const char* message);
	using FnGroupMsg = int(DICE_EVCALL*)(int msgId, long long fromGID, long long fromUID, const char* message);
	using FnMemberChange = int(DICE_EVCALL*)(long long fromGID, long long a, long long b);
	using FnGroupBan = int(DICE_EVCALL*)(long long fromGID, long long beingOperateQQ, long long operatorQQ, const char* duration);
	using FnGroupInvited = int(DICE_EVCALL*)(long long fromGID, long long fromUID);
	using FnFriendRequest = int(DICE_EVCALL*)(long long fromUID, const char* message);
	using FnFriendAdded = int(DICE_EVCALL*)(long long fromUID);
	using FnExtra = int(DICE_EVCALL*)(const char* jsonData);

	void* handle = nullptr;
	FnStartup startUp = nullptr;
	FnVoid enable = nullptr;
	FnVoid exit = nullptr;
	FnPrivateMsg privateMsg = nullptr;
	FnGroupMsg groupMsg = nullptr;
	FnMemberChange memberAdd = nullptr;     // (gid, uid, operator)
	FnMemberChange memberKicked = nullptr;  // (gid, beingKicked, operator)
	FnGroupBan groupBan = nullptr;
	FnGroupInvited groupInvited = nullptr;
	FnFriendRequest friendRequest = nullptr;
	FnFriendAdded friendAdded = nullptr;
	FnExtra extra = nullptr;

	bool load(const std::filesystem::path& p, std::string& err);
};
