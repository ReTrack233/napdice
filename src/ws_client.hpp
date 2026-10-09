// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
//
// Minimal RFC 6455 WebSocket client (ws:// only), enough for talking to a local OneBot server.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class WsClient {
public:
	using MessageHandler = std::function<void(std::string&&)>;
	using CloseHandler = std::function<void(const std::string& reason)>;

	WsClient();
	~WsClient();
	WsClient(const WsClient&) = delete;
	WsClient& operator=(const WsClient&) = delete;

	// Blocking connect + handshake. Starts the reader thread on success.
	bool connect(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
				 int timeout_ms, std::string& err);
	// Thread-safe. Returns false when the connection is not open or the write failed.
	bool send_text(const std::string& data);
	// Close the connection (thread-safe, idempotent) and join the reader thread.
	void close();
	bool is_open() const { return open_.load(); }

	void set_on_message(MessageHandler h) { onMessage_ = std::move(h); }
	void set_on_close(CloseHandler h) { onClose_ = std::move(h); }

private:
	bool send_frame(unsigned char opcode, const char* data, size_t len);
	bool read_exact(char* buf, size_t len);
	void reader_loop();
	void shutdown_socket();

	intptr_t sock_ = -1;
	std::atomic<bool> open_{false};
	std::atomic<bool> closing_{false};
	std::mutex writeMu_;
	std::mutex closeMu_;
	std::thread reader_;
	MessageHandler onMessage_;
	CloseHandler onClose_;
	std::string pending_;  // bytes read past the HTTP handshake
};
