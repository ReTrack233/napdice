# Dice! 核心补丁

本目录中的补丁用于修复 Dice! 核心（[Dice-Developer-Team/Dice](https://github.com/Dice-Developer-Team/Dice)，newdev 分支）中的问题。CI 在编译核心前依次应用这些补丁；若 `git apply --check` 失败（通常表示上游已修复或相关代码已变更），则跳过该补丁并给出警告。

以下问题均在使用真实核心进行端到端测试时发现，尚未提交至上游。

## 0001-json-brace-init-copy

文件：`Dice/DiceAttrVar.cpp`，函数 `AnysTable::from_json`

`fifo_json tab{j};` 意在复制参数 `j`。按 C++ 列表初始化规则，GCC 与 Clang 会优先选用 `initializer_list` 构造函数，得到一个仅包含 `j` 的数组。随后函数将该数组的元素再次转换为表，形成无限递归，最终因栈溢出崩溃。

受影响的场景包括核心将 JSON 对象转换为内部表结构的各处，例如重启后读取当日统计数据、`askExtra` 返回对象等。

修改为 `fifo_json tab = j;`。该写法在各编译器下均为复制，不改变原有语义。MSVC 编译的核心是否受此问题影响尚未验证。

## 0002-askextra-table-to-json

文件：`Dice/DiceLua.cpp`，函数 `askExtra`

`to_json(lua_to_table(L, 1))` 中，`lua_to_table` 返回 `AnysTable`，但 `to_json` 没有接受 `AnysTable` 的重载。由于 `chatInfo` 存在以 `AnysTable` 为参数的非显式构造函数，该调用被解析为 `to_json(const chatInfo&)`，结果只保留 `uid`、`gid`、`chid` 中非零的字段（通常为 `null`），其余参数全部丢失。

该问题与编译器无关。未应用此补丁时，Lua 脚本以 table 调用 `askExtra` 无法传递参数，应改为传入 JSON 字符串。

修改为 `lua_to_table(L, 1).to_json()`。

## 0003-msvc-randint-static-init

文件：`Dice/RandomGenerator.cpp`

该文件用 `defined(__i386__) || defined(__x86_64__)` 判断 x86 平台：满足时每次调用 `Randint` 都以 `__rdtsc()` 为种子新建局部 `std::mt19937`，否则使用一个全局的 `std::mt19937 gen`。这两个宏只有 GCC、Clang（含 clang-cl）定义，MSVC 使用的是 `_M_IX86`、`_M_X64`，因此 MSVC 编译时走的是全局 `gen` 分支。文件内 `#ifdef _MSC_VER` 引入 `<intrin.h>` 的写法说明，原意是让 MSVC 走 x86 分支。

全局 `gen` 需要动态初始化。全局对象 `console`（`Dice/DiceConsole.cpp`）在构造时调用 `genKey` 生成 `authkey_pub`、`authkey_pri`，而 MSVC 按目标文件的链接顺序执行动态初始化，DiceConsole 排在 RandomGenerator 之前。此时 `gen` 尚未初始化，内部状态全为 0，输出恒为 0；MSVC 的 `uniform_int_distribution` 采用拒绝采样，会一直拒绝 0，于是陷入死循环。该循环发生在 `LoadLibrary` 内部、持有加载器锁期间，表现为进程卡在加载核心这一步，其他线程也无法创建或退出。

在 Windows CI 的端到端测试中，用调试器抓取卡住时的调用栈确认了该问题：

```
w4123_Dice!RandomGenerator::Randint
w4123_Dice!RandomGenerator::genKey
w4123_Dice!`dynamic initializer for 'console''
w4123_Dice!_initterm
...
KERNELBASE!LoadLibraryExW
```

修改为在三处条件中加入 `defined(_M_IX86) || defined(_M_X64)`，使 MSVC 与 GCC、Clang 走同一分支。GCC、Clang 编译的核心不受影响。
