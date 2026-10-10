// 0004-local-jrrp 补丁中取值逻辑（Dice/DiceJrrp.h）的单元测试。
// 在打过补丁的 Dice! 源码目录外编译运行：
//   g++ -std=c++17 -I <Dice源码>/Dice patches/dice-core/tests/jrrp_test.cpp -o jrrp_test && ./jrrp_test
// CI 中通过同目录的 CMakeLists.txt 编译。
#include "DiceJrrp.h"
#include <cstdio>
#include <random>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
	// 固定返回值的“随机数”，同时记录被请求的区间
	int asked_lo = 0, asked_hi = 0;
	auto fixed = [&](int v) {
		return [&, v](int lo, int hi) { asked_lo = lo; asked_hi = hi; return v < lo ? lo : (v > hi ? hi : v); };
	};
	const jrrp::Params def{};  // 1~100，低值线 20，连续 2 次，补偿下限 50

	// 1. 普通情况：在 [JrrpMin, JrrpMax] 里取值
	{
		auto r = jrrp::roll(def, 0, fixed(77));
		CHECK(r.value == 77 && !r.pity && r.next_streak == 0);
		CHECK(asked_lo == 1 && asked_hi == 100);
	}
	// 2. 分布区间可任意设置，填反了自动对调
	{
		jrrp::Params p = def; p.min = 30; p.max = 120;
		jrrp::roll(p, 0, fixed(60));
		CHECK(asked_lo == 30 && asked_hi == 120);
		p.min = 120; p.max = 30;
		jrrp::roll(p, 0, fixed(60));
		CHECK(asked_lo == 30 && asked_hi == 120);
		p.min = 0; p.max = 0;
		auto r = jrrp::roll(p, 0, fixed(0));
		CHECK(r.value == 0 && asked_lo == 0 && asked_hi == 0);
	}
	// 3. 低值累计：不高于 JrrpLowLine 记一次，高于则清零
	{
		auto r = jrrp::roll(def, 0, fixed(20));
		CHECK(r.value == 20 && r.next_streak == 1);
		r = jrrp::roll(def, 1, fixed(5));
		CHECK(!r.pity && r.next_streak == 2);
		r = jrrp::roll(def, 1, fixed(21));
		CHECK(r.next_streak == 0);
	}
	// 4. 连续低值达到 JrrpPityDays：在 [JrrpPityMin, JrrpMax] 里取值
	{
		auto r = jrrp::roll(def, 2, fixed(3));
		CHECK(r.pity && r.streak == 2);
		CHECK(asked_lo == 50 && asked_hi == 100);
		CHECK(r.value == 50 && r.next_streak == 0);
		r = jrrp::roll(def, 5, fixed(90));
		CHECK(r.pity && r.value == 90);
		jrrp::Params p = def; p.max = 80;
		r = jrrp::roll(p, 2, fixed(3));
		CHECK(r.pity && asked_lo == 50 && asked_hi == 80);
	}
	// 5. 关闭：JrrpPityDays = 0，或 JrrpLowLine = 0
	{
		jrrp::Params p = def; p.pity_days = 0;
		auto r = jrrp::roll(p, 9, fixed(4));
		CHECK(!r.pity && r.value == 4 && r.next_streak == 10);
		p = def; p.low_line = 0;
		r = jrrp::roll(p, 9, fixed(4));
		CHECK(!r.pity && r.next_streak == 0);
	}
	// 6. 补偿下限超出分布区间时，夹回区间内
	{
		jrrp::Params p = def; p.pity_min = 200;
		auto r = jrrp::roll(p, 3, fixed(1));
		CHECK(r.pity && asked_lo == 100 && r.value == 100);
		p = def; p.min = 70; p.pity_min = 30;
		r = jrrp::roll(p, 3, fixed(1));
		CHECK(r.pity && asked_lo == 70);
		r = jrrp::roll(def, -4, fixed(60));  // 读到负数的连续次数按 0 处理
		CHECK(!r.pity && r.streak == 0);
	}
	// 7. 用真随机数模拟 1000 人各 365 天：默认参数下，连续低值最多 2 次
	{
		std::mt19937 gen(20261010);
		auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(gen); };
		int worst = 0, pity_cnt = 0;
		long long sum = 0;
		for (int u = 0; u < 1000; ++u) {
			int streak = 0;
			for (int d = 0; d < 365; ++d) {
				auto r = jrrp::roll(def, streak, rnd);
				CHECK(r.value >= 1 && r.value <= 100);
				if (r.pity) { ++pity_cnt; CHECK(r.value >= 50); }
				sum += r.value;
				streak = r.next_streak;
				if (streak > worst) worst = streak;
			}
		}
		CHECK(worst == 2);
		std::printf("simulation: max low streak %d, raised %d of 365000 rolls, mean %.2f\n", worst, pity_cnt, sum / 365000.0);
	}
	if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
	std::printf("all jrrp checks passed\n");
	return 0;
}
