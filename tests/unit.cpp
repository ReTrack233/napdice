// Unit tests for the platform-sensitive helpers (runs on Windows CI and Linux).
#include <cstdio>
#include <fstream>
#include <string>

#include "config.hpp"
#include "cq.hpp"
#include "platform.hpp"

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_fail;                                                   \
		}                                                               \
	} while (0)
#define CHECK_EQ(a, b)                                                                                     \
	do {                                                                                                   \
		auto va = (a);                                                                                     \
		auto vb = (b);                                                                                     \
		if (!(va == vb)) {                                                                                 \
			std::printf("FAIL %s:%d: %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, #a, \
						std::string(va).c_str(), std::string(vb).c_str());                                 \
			++g_fail;                                                                                      \
		}                                                                                                  \
	} while (0)

static void write_file(const fs::path& p, const std::string& s) {
	fs::create_directories(p.parent_path());
	std::ofstream f(p, std::ios::binary);
	f << s;
}

int main() {
	plat::init_console();
	const fs::path tmp = fs::temp_directory_path() / plat::u8path("napdice-unit-骰子");
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp);

	// ---- incoming normalisation
	CHECK_EQ(cq::normalize_incoming("[CQ:reply,id=5][CQ:at,qq=10000] .r 1d6", true), std::string("[CQ:at,qq=10000] .r 1d6"));
	CHECK_EQ(cq::normalize_incoming("[CQ:reply,id=5] .r", false), std::string("[CQ:reply,id=5] .r"));
	CHECK_EQ(cq::normalize_incoming("[CQ:at,qq=1,name=@某人].r", true), std::string("[CQ:at,qq=1].r"));
	CHECK_EQ(cq::normalize_incoming(".r 1d100 &#91;注释&#93;", true), std::string(".r 1d100 &#91;注释&#93;"));
	CHECK_EQ(cq::normalize_incoming("半个 [CQ:at,qq=1", true), std::string("半个 [CQ:at,qq=1"));

	// ---- escaping
	CHECK_EQ(cq::escape_param("a,b[c]&d"), std::string("a&#44;b&#91;c&#93;&amp;d"));
	CHECK_EQ(cq::unescape(cq::escape_param("a,b[c]&d")), std::string("a,b[c]&d"));

	// ---- outgoing rewrite
	const fs::path root = tmp / plat::u8path("根 目录");
	write_file(root / "data" / "image" / plat::u8path("图,1.png"), "abc");
	write_file(root / "data" / "record" / "v.silk", "xyz");
	cq::MediaDirs dirs{root, root / "Dice10000", false};
	CHECK_EQ(cq::rewrite_outgoing("[CQ:at,id=123]你好", dirs), std::string("[CQ:at,qq=123]你好"));
	CHECK_EQ(cq::rewrite_outgoing("[CQ:at,qq=123]", dirs), std::string("[CQ:at,qq=123]"));
	{
		std::string out = cq::rewrite_outgoing("看[CQ:image,file=图&#44;1.png]图", dirs);
		std::string abs = cq::escape_param(plat::path_u8(fs::absolute(root / "data" / "image" / plat::u8path("图,1.png")).lexically_normal()));
		CHECK_EQ(out, "看[CQ:image,file=" + abs + "]图");
	}
	CHECK_EQ(cq::rewrite_outgoing("[CQ:image,file=https://x/y.png]", dirs), std::string("[CQ:image,file=https://x/y.png]"));
	CHECK_EQ(cq::rewrite_outgoing("[CQ:image,file=nope.png]", dirs), std::string("[CQ:image,file=nope.png]"));
	cq::MediaDirs b64{root, root / "Dice10000", true};
	CHECK_EQ(cq::rewrite_outgoing("[CQ:record,file=v.silk]", b64), std::string("[CQ:record,file=base64://eHl6]"));
	CHECK_EQ(cq::rewrite_outgoing("[CQ:image,file=图&#44;1.png]", b64), std::string("[CQ:image,file=base64://YWJj]"));

	// ---- paths
	{
		std::string s = "骰子/测试 1.png";
		CHECK_EQ(plat::path_u8(plat::u8path(s)), s);
		// operator/ inserts the preferred separator ('\' on Windows), so compare as paths, not strings.
		CHECK(plat::u8path(s) == fs::path(plat::u8path("骰子")) / plat::u8path("测试 1.png"));
		bool lossless = false;
		std::string n = plat::utf8_to_native("C:/Dice/abc", lossless);
		CHECK(lossless);
		CHECK_EQ(n, std::string("C:/Dice/abc"));
	}
#ifdef _WIN32
	CHECK(plat::pe_bits(plat::exe_path()) == plat::self_bits());
#else
	CHECK(plat::pe_bits(plat::exe_path()) == 0);
#endif
	CHECK(plat::pe_bits(tmp / "missing.dll") == -1);

	// ---- config (BOM + comments)
	{
		fs::path cfgFile = tmp / "cfg.json";
		write_file(cfgFile, "\xEF\xBB\xBF// c\n{\n  \"ws_url\": \"ws://1.2.3.4:5/\", // x\n  \"workers\": 999,\n  \"root_dir\": \"数据\"\n}\n");
		Config cfg;
		std::string err;
		bool created = true;
		CHECK(load_config(cfgFile, cfg, err, created));
		CHECK(!created);
		CHECK_EQ(cfg.ws_url, std::string("ws://1.2.3.4:5/"));
		CHECK(cfg.workers == 64);
		CHECK_EQ(plat::path_u8(cfg.root.filename()), std::string("数据"));
		write_file(cfgFile, "{ broken");
		Config bad;
		CHECK(!load_config(cfgFile, bad, err, created));
		fs::path fresh = tmp / "new.json";
		Config def;
		CHECK(load_config(fresh, def, err, created));
		CHECK(created && fs::exists(fresh));
		CHECK_EQ(def.ws_url, std::string("ws://127.0.0.1:3001"));
	}

	fs::remove_all(tmp, ec);
	std::printf(g_fail ? "%d check(s) failed\n" : "all unit checks passed\n", g_fail);
	return g_fail ? 1 : 0;
}
