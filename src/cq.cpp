// NapDice - Dice! driver for NapCat (OneBot 11)
// Copyright (C) 2026 NapDice contributors. Licensed under AGPL-3.0-or-later.
#include "cq.hpp"

#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "platform.hpp"

namespace fs = std::filesystem;

namespace cq {

static void replace_all(std::string& s, const std::string& from, const std::string& to) {
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
}

std::string escape_param(const std::string& in) {
	std::string s = in;
	replace_all(s, "&", "&amp;");
	replace_all(s, "[", "&#91;");
	replace_all(s, "]", "&#93;");
	replace_all(s, ",", "&#44;");
	return s;
}

std::string file_to_base64_uri(const std::filesystem::path& p) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return {};
	std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out = "base64://";
	out.reserve(out.size() + (data.size() + 2) / 3 * 4);
	size_t i = 0;
	const auto* d = reinterpret_cast<const unsigned char*>(data.data());
	for (; i + 2 < data.size(); i += 3) {
		unsigned v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += tbl[(v >> 6) & 63];
		out += tbl[v & 63];
	}
	if (i + 1 == data.size()) {
		unsigned v = d[i] << 16;
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += "==";
	}
	else if (i + 2 == data.size()) {
		unsigned v = (d[i] << 16) | (d[i + 1] << 8);
		out += tbl[(v >> 18) & 63];
		out += tbl[(v >> 12) & 63];
		out += tbl[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

std::string unescape(const std::string& in) {
	std::string s = in;
	replace_all(s, "&#91;", "[");
	replace_all(s, "&#93;", "]");
	replace_all(s, "&#44;", ",");
	replace_all(s, "&amp;", "&");
	return s;
}

namespace {

struct Code {
	std::string type;
	std::vector<std::pair<std::string, std::string>> params;  // raw (still escaped) values
};

bool parse_code(const std::string& body, Code& c) {
	// body: "type,k=v,k=v" (without "[CQ:" and "]")
	size_t pos = body.find(',');
	c.type = body.substr(0, pos);
	if (c.type.empty()) return false;
	while (pos != std::string::npos) {
		size_t next = body.find(',', pos + 1);
		std::string kv = body.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
		size_t eq = kv.find('=');
		if (eq == std::string::npos) c.params.emplace_back(kv, "");
		else c.params.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
		pos = next;
	}
	return true;
}

std::string build_code(const Code& c) {
	std::string s = "[CQ:" + c.type;
	for (const auto& [k, v] : c.params) s += "," + k + "=" + v;
	return s + "]";
}

std::string* find_param(Code& c, const std::string& key) {
	for (auto& [k, v] : c.params)
		if (k == key) return &v;
	return nullptr;
}

bool has_scheme(const std::string& v) {
	static const char* schemes[] = {"http://", "https://", "file:", "base64://", "data:"};
	for (const char* s : schemes)
		if (v.rfind(s, 0) == 0) return true;
	return false;
}

// Apply `fn(Code&) -> bool changed` to every CQ code in `msg`.
template <typename Fn>
std::string transform_codes(const std::string& msg, Fn&& fn) {
	std::string out;
	out.reserve(msg.size());
	size_t pos = 0;
	while (pos < msg.size()) {
		size_t start = msg.find("[CQ:", pos);
		if (start == std::string::npos) {
			out.append(msg, pos, std::string::npos);
			break;
		}
		size_t end = msg.find(']', start);
		if (end == std::string::npos) {
			out.append(msg, pos, std::string::npos);
			break;
		}
		out.append(msg, pos, start - pos);
		Code c;
		std::string body = msg.substr(start + 4, end - start - 4);
		if (parse_code(body, c) && fn(c)) out += build_code(c);
		else out.append(msg, start, end - start + 1);
		pos = end + 1;
	}
	return out;
}

}  // namespace

std::string normalize_incoming(const std::string& raw, bool strip_reply) {
	std::string s = raw;
	if (strip_reply && s.rfind("[CQ:reply,", 0) == 0) {
		size_t end = s.find(']');
		if (end != std::string::npos) {
			s.erase(0, end + 1);
			size_t first = s.find_first_not_of(" \t\r\n");
			s.erase(0, first == std::string::npos ? s.size() : first);
		}
	}
	return transform_codes(s, [](Code& c) {
		if (c.type != "at") return false;
		std::string* qq = find_param(c, "qq");
		if (!qq) qq = find_param(c, "id");
		if (!qq || (c.params.size() == 1 && c.params[0].first == "qq")) return false;
		std::string v = *qq;
		c.params.clear();
		c.params.emplace_back("qq", v);
		return true;
	});
}

std::string rewrite_outgoing(const std::string& msg, const MediaDirs& dirs) {
	if (msg.find("[CQ:") == std::string::npos) return msg;
	return transform_codes(msg, [&](Code& c) {
		if (c.type == "at") {
			if (!find_param(c, "qq")) {
				if (std::string* id = find_param(c, "id")) {
					for (auto& [k, v] : c.params)
						if (k == "id") k = "qq";
					(void)id;
					return true;
				}
			}
			return false;
		}
		if (c.type != "image" && c.type != "record" && c.type != "video" && c.type != "file") return false;
		std::string* file = find_param(c, "file");
		if (!file || file->empty()) return false;
		const std::string value = unescape(*file);
		if (has_scheme(value)) return false;
		fs::path p = plat::u8path(value);
		std::error_code ec;
		if (p.is_absolute()) {
			if (!dirs.base64 || !fs::exists(p, ec)) return false;
			std::string b64 = file_to_base64_uri(p);
			if (b64.empty()) return false;
			*file = b64;
			return true;
		}
		std::vector<fs::path> candidates;
		if (c.type == "image") candidates.push_back(dirs.root / "data" / "image" / p);
		else if (c.type == "record") candidates.push_back(dirs.root / "data" / "record" / p);
		else if (c.type == "video") candidates.push_back(dirs.root / "data" / "video" / p);
		candidates.push_back(dirs.root / p);
		if (!dirs.dice.empty()) candidates.push_back(dirs.dice / p);
		for (const auto& cand : candidates) {
			if (fs::exists(cand, ec)) {
				if (dirs.base64) {
					std::string b64 = file_to_base64_uri(cand);
					if (b64.empty()) return false;
					*file = b64;  // base64 alphabet needs no CQ escaping
				}
				else {
					*file = escape_param(plat::path_u8(fs::absolute(cand, ec).lexically_normal()));
				}
				return true;
			}
		}
		return false;
	});
}

} // namespace cq
