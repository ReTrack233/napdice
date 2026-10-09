// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#include "ws_client.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t kBadSock = INVALID_SOCKET;
#define ND_CLOSESOCK closesocket
#define ND_SEND_FLAGS 0
namespace {
struct WsaInit {
	WsaInit() {
		WSADATA d;
		WSAStartup(MAKEWORD(2, 2), &d);
	}
	~WsaInit() { WSACleanup(); }
} g_wsaInit;
}  // namespace
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kBadSock = -1;
#define ND_CLOSESOCK ::close
#define ND_SEND_FLAGS MSG_NOSIGNAL
#endif

namespace {

std::mt19937& rng() {
	thread_local std::mt19937 g{std::random_device{}()};
	return g;
}

std::string base64(const unsigned char* data, size_t len) {
	static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	size_t i = 0;
	while (i + 2 < len) {
		uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += tbl[(v >> 6) & 63];
		out += tbl[v & 63];
		i += 3;
	}
	if (i + 1 == len) {
		uint32_t v = data[i] << 16;
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += "==";
	}
	else if (i + 2 == len) {
		uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += tbl[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

bool set_nonblocking(sock_t s, bool nb) {
#ifdef _WIN32
	u_long mode = nb ? 1 : 0;
	return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0) return false;
	return fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

// Wait until the socket is readable (or writable); returns >0 ready, 0 timeout, <0 error.
int wait_sock(sock_t s, bool write, int timeout_ms) {
#ifdef _WIN32
	// select() rather than WSAPoll: older Windows 10 builds never report a failed connect via WSAPoll.
	fd_set main{}, err{};
	FD_ZERO(&main);
	FD_ZERO(&err);
	FD_SET(s, &main);
	FD_SET(s, &err);
	timeval tv{};
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	int r = ::select(0, write ? nullptr : &main, write ? &main : nullptr, &err, &tv);
	if (r > 0 && FD_ISSET(s, &err) && !FD_ISSET(s, &main)) return write ? 1 : -1;  // connect failed: SO_ERROR says why
	return r;
#else
	pollfd p{};
	p.fd = s;
	p.events = write ? POLLOUT : POLLIN;
	return ::poll(&p, 1, timeout_ms);
#endif
}

struct Url {
	std::string host, port, path, hostHeader;
};

bool parse_url(const std::string& url, Url& u, std::string& err) {
	std::string rest;
	if (url.rfind("ws://", 0) == 0) rest = url.substr(5);
	else if (url.rfind("wss://", 0) == 0) {
		err = "暂不支持 wss://，连接本机 NapCat 请使用 ws://";
		return false;
	}
	else {
		err = "ws_url 必须以 ws:// 开头";
		return false;
	}
	size_t slash = rest.find('/');
	std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
	u.path = slash == std::string::npos ? "/" : rest.substr(slash);
	if (hostport.empty()) {
		err = "ws_url 缺少主机名";
		return false;
	}
	if (hostport[0] == '[') {  // IPv6 literal
		size_t rb = hostport.find(']');
		if (rb == std::string::npos) {
			err = "ws_url 中的 IPv6 地址格式错误";
			return false;
		}
		u.host = hostport.substr(1, rb - 1);
		u.port = (rb + 1 < hostport.size() && hostport[rb + 1] == ':') ? hostport.substr(rb + 2) : "80";
	}
	else {
		size_t colon = hostport.rfind(':');
		u.host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
		u.port = colon == std::string::npos ? "80" : hostport.substr(colon + 1);
	}
	u.hostHeader = hostport;
	return true;
}

}  // namespace

WsClient::WsClient() = default;

WsClient::~WsClient() { close(); }

bool WsClient::connect(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
					   int timeout_ms, std::string& err) {
	Url u;
	if (!parse_url(url, u, err)) return false;

	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res) {
		err = "无法解析地址 " + u.host;
		return false;
	}
	sock_t s = kBadSock;
	for (addrinfo* ai = res; ai; ai = ai->ai_next) {
		s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == kBadSock) continue;
		set_nonblocking(s, true);
		int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
		bool ok = rc == 0;
		if (!ok) {
#ifdef _WIN32
			bool inProgress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
			bool inProgress = errno == EINPROGRESS;
#endif
			if (inProgress && wait_sock(s, true, timeout_ms) > 0) {
				int soErr = 0;
				socklen_t len = sizeof(soErr);
				getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
				ok = soErr == 0;
			}
		}
		if (ok) {
			set_nonblocking(s, false);
			break;
		}
		ND_CLOSESOCK(s);
		s = kBadSock;
	}
	freeaddrinfo(res);
	if (s == kBadSock) {
		err = "连接 " + u.hostHeader + " 失败（NapCat 没启动，或没有开启这个端口的 WebSocket 服务器？）";
		return false;
	}
	int one = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

	unsigned char keyRaw[16];
	for (auto& b : keyRaw) b = static_cast<unsigned char>(rng()() & 0xFF);
	std::string req = "GET " + u.path + " HTTP/1.1\r\n";
	req += "Host: " + u.hostHeader + "\r\n";
	req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
	req += "Sec-WebSocket-Key: " + base64(keyRaw, sizeof(keyRaw)) + "\r\n";
	req += "Sec-WebSocket-Version: 13\r\n";
	for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
	req += "\r\n";
	size_t sent = 0;
	while (sent < req.size()) {
		int n = ::send(s, req.data() + sent, static_cast<int>(req.size() - sent), ND_SEND_FLAGS);
		if (n <= 0) {
			ND_CLOSESOCK(s);
			err = "发送握手请求失败";
			return false;
		}
		sent += static_cast<size_t>(n);
	}

	std::string resp;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	size_t hdrEnd = std::string::npos;
	char buf[4096];
	while (hdrEnd == std::string::npos) {
		int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count());
		if (left <= 0 || wait_sock(s, false, left) <= 0) {
			ND_CLOSESOCK(s);
			err = "等待握手响应超时";
			return false;
		}
		int n = ::recv(s, buf, sizeof(buf), 0);
		if (n <= 0) {
			ND_CLOSESOCK(s);
			err = "握手时连接被关闭（检查 access_token 或 NapCat 日志）";
			return false;
		}
		resp.append(buf, static_cast<size_t>(n));
		hdrEnd = resp.find("\r\n\r\n");
		if (resp.size() > 65536) break;
	}
	if (hdrEnd == std::string::npos) {
		ND_CLOSESOCK(s);
		err = "握手响应无效";
		return false;
	}
	std::string statusLine = resp.substr(0, resp.find("\r\n"));
	if (statusLine.find(" 101") == std::string::npos) {
		ND_CLOSESOCK(s);
		if (statusLine.find(" 401") != std::string::npos || statusLine.find(" 403") != std::string::npos)
			err = "NapCat 拒绝连接：access_token 不正确（" + statusLine + "）";
		else
			err = "握手失败：" + statusLine;
		return false;
	}
	pending_ = resp.substr(hdrEnd + 4);
	sock_ = static_cast<intptr_t>(s);
	closing_ = false;
	open_ = true;
	reader_ = std::thread([this] { reader_loop(); });
	return true;
}

bool WsClient::read_exact(char* out, size_t len) {
	size_t got = 0;
	if (!pending_.empty()) {
		size_t take = std::min(len, pending_.size());
		std::memcpy(out, pending_.data(), take);
		pending_.erase(0, take);
		got = take;
	}
	sock_t s = static_cast<sock_t>(sock_);
	while (got < len) {
		size_t want = len - got;
		if (want > (1u << 20)) want = (1u << 20);
		int n = ::recv(s, out + got, static_cast<int>(want), 0);
		if (n <= 0) return false;
		got += static_cast<size_t>(n);
	}
	return true;
}

void WsClient::reader_loop() {
	std::string reason = "连接已断开";
	std::string message;
	bool inMessage = false;
	std::string payload;
	for (;;) {
		unsigned char h[2];
		if (!read_exact(reinterpret_cast<char*>(h), 2)) break;
		const bool fin = (h[0] & 0x80) != 0;
		const unsigned char op = h[0] & 0x0F;
		const bool masked = (h[1] & 0x80) != 0;
		uint64_t len = h[1] & 0x7F;
		if (len == 126) {
			unsigned char e[2];
			if (!read_exact(reinterpret_cast<char*>(e), 2)) break;
			len = (static_cast<uint64_t>(e[0]) << 8) | e[1];
		}
		else if (len == 127) {
			unsigned char e[8];
			if (!read_exact(reinterpret_cast<char*>(e), 8)) break;
			len = 0;
			for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
		}
		if (len > (256ull << 20)) {
			reason = "收到过大的数据帧";
			break;
		}
		unsigned char mask[4]{};
		if (masked && !read_exact(reinterpret_cast<char*>(mask), 4)) break;
		payload.resize(static_cast<size_t>(len));
		if (len && !read_exact(payload.data(), static_cast<size_t>(len))) break;
		if (masked) {
			for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
		}
		if (op == 0x8) {  // close
			reason = "对端关闭了连接";
			if (payload.size() >= 2) {
				int code = (static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]);
				reason += "（" + std::to_string(code) + (payload.size() > 2 ? " " + payload.substr(2) : "") + "）";
			}
			if (!closing_) send_frame(0x8, payload.data(), payload.size() >= 2 ? 2 : 0);
			break;
		}
		if (op == 0x9) {  // ping
			send_frame(0xA, payload.data(), payload.size());
			continue;
		}
		if (op == 0xA) continue;  // pong
		if (op == 0x1 || op == 0x2) {
			message = std::move(payload);
			payload.clear();
			inMessage = true;
		}
		else if (op == 0x0) {
			if (!inMessage) continue;
			message += payload;
		}
		else {
			continue;  // unknown opcode
		}
		if (fin && inMessage) {
			inMessage = false;
			if (onMessage_) onMessage_(std::move(message));
			message.clear();
		}
	}
	const bool wasOpen = open_.exchange(false);
	shutdown_socket();
	if (closing_) reason = "主动断开";
	if (wasOpen && onClose_) onClose_(reason);
}

bool WsClient::send_frame(unsigned char opcode, const char* data, size_t len) {
	std::string frame;
	frame.reserve(len + 14);
	frame.push_back(static_cast<char>(0x80 | opcode));
	if (len < 126) {
		frame.push_back(static_cast<char>(0x80 | len));
	}
	else if (len <= 0xFFFF) {
		frame.push_back(static_cast<char>(0x80 | 126));
		frame.push_back(static_cast<char>((len >> 8) & 0xFF));
		frame.push_back(static_cast<char>(len & 0xFF));
	}
	else {
		frame.push_back(static_cast<char>(0x80 | 127));
		for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (8 * i)) & 0xFF));
	}
	unsigned char mask[4];
	uint32_t r = rng()();
	std::memcpy(mask, &r, 4);
	frame.append(reinterpret_cast<char*>(mask), 4);
	size_t start = frame.size();
	frame.append(data, len);
	for (size_t i = 0; i < len; ++i) frame[start + i] = static_cast<char>(frame[start + i] ^ mask[i & 3]);

	std::lock_guard<std::mutex> lk(writeMu_);
	if (sock_ == -1) return false;
	sock_t s = static_cast<sock_t>(sock_);
	size_t sent = 0;
	while (sent < frame.size()) {
		size_t chunk = frame.size() - sent;
		if (chunk > (1u << 20)) chunk = (1u << 20);
		int n = ::send(s, frame.data() + sent, static_cast<int>(chunk), ND_SEND_FLAGS);
		if (n <= 0) return false;
		sent += static_cast<size_t>(n);
	}
	return true;
}

bool WsClient::send_text(const std::string& data) {
	if (!open_) return false;
	return send_frame(0x1, data.data(), data.size());
}

void WsClient::shutdown_socket() {
	std::lock_guard<std::mutex> lk(writeMu_);
	if (sock_ != -1) {
#ifdef _WIN32
		::shutdown(static_cast<sock_t>(sock_), SD_BOTH);
#else
		::shutdown(static_cast<sock_t>(sock_), SHUT_RDWR);
#endif
	}
}

void WsClient::close() {
	std::lock_guard<std::mutex> lk(closeMu_);
	closing_ = true;
	if (open_) {
		const char code[2] = {0x03, static_cast<char>(0xE8)};  // 1000 normal closure
		send_frame(0x8, code, 2);
	}
	shutdown_socket();
	if (reader_.joinable()) {
		if (reader_.get_id() == std::this_thread::get_id()) reader_.detach();
		else reader_.join();
	}
	std::lock_guard<std::mutex> wk(writeMu_);
	if (sock_ != -1) {
		ND_CLOSESOCK(static_cast<sock_t>(sock_));
		sock_ = -1;
	}
	open_ = false;
}
