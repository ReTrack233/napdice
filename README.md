# NapDice

[![License](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
[![OneBot](https://img.shields.io/badge/OneBot-v11-black.svg)](https://github.com/botuniverse/onebot-11)

Dice! 的 OneBot 11 驱动，用于在 NapCat 上直接运行 Dice! 骰娘。

## 简介

NapDice 提供 `dice-host` 程序。该程序加载官方 Dice! 核心（`w4123.Dice.dll`），通过正向 WebSocket 连接 NapCat，并实现 Dice! 的驱动接口（DD API），作用与 DiceDriver.OneBot 相同。

本项目不修改 Dice! 核心的行为，原有 `Dice{QQ号}` 目录下的配置、Lua 脚本、模块、牌堆及用户数据均可直接沿用。

注：NapCat 自 v4.18.6 起仅加载官方白名单内的插件，因此本项目以独立进程的方式运行，通过 OneBot WebSocket 接入，不依赖 NapCat 插件系统。

## 兼容性

### 运行环境

- [x] Windows x64
- [x] Windows x86（用于加载 32 位核心）
- [ ] Linux / macOS（可从源码编译，暂不提供发布包）

### 协议端

- [x] NapCat（正向 WebSocket）

其他 OneBot 11 实现理论上可用，但未经测试。

### 驱动接口

- [x] 群聊、私聊及群临时会话消息
- [x] 好友列表、群列表、群成员信息查询
- [x] 入群、被踢、禁言事件
- [x] 好友申请、入群邀请的处理
- [x] 踢人、禁言、设置群名片与头衔、退群
- [x] 上传群文件、发送私聊文件
- [x] 通过 `askExtra` 调用 OneBot API
- [x] 戳一戳事件
- [ ] QQ 频道、讨论组
- [ ] 云黑名单上报、在线更新

## 下载

前往 [Releases](../../releases) 页面下载 `NapDice-<版本>-win-x64.zip` 或 `NapDice-<版本>-win-x86.zip`。压缩包内包含 `dice-host.exe` 及相同位数的 Dice! 核心。

## 快速开始

1. 在 NapCat WebUI 的「网络配置」中新建「WebSocket 服务器」，主机填写 `127.0.0.1`，端口填写 `3001`，并设置 Token。
2. 运行 `dice-host.exe`。首次运行时，程序会在同一目录下生成配置文件 `dice-host.json`。
3. 将 `dice-host.json` 中的 `ws_url` 和 `access_token` 修改为第 1 步的设置，然后重新运行。
4. 按日志中给出的 `.master` 口令私聊骰娘，完成认主。

程序日志保存在 `logs` 目录。关闭窗口或按 Ctrl+C 时，程序会在保存数据后退出。

## 从 DiceDriver.OneBot 迁移

1. 备份 `Dice{QQ号}` 目录，并关闭 DiceDriver.OneBot。
2. 将发布包中的文件复制到原 `DiceDriver.OneBot.exe` 所在目录。也可以将文件放在其他位置，并在 `dice-host.json` 中将 `root_dir` 设置为该目录。
3. 原先供 DiceDriver.OneBot 使用的 NapCat WebSocket 服务器可以继续使用。

如需继续使用原有的核心文件，请在 `core_path` 中指定其路径，并使用相同位数的 `dice-host.exe`。`w4123.Dice.windows.i386.dll` 对应 x86 版本，`w4123.Dice.windows.amd64.dll` 对应 x64 版本。位数不一致时，程序会给出提示并退出。

## 配置

配置文件为 `dice-host.json`，允许使用 `//` 注释。

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `ws_url` | `ws://127.0.0.1:3001` | NapCat 正向 WebSocket 地址，仅支持 `ws://` |
| `access_token` | 空 | NapCat 中设置的 Token |
| `core_path` | 空 | 核心文件路径。为空时，依次在程序目录、`root_dir`、`root_dir/Diceki` 中查找位数匹配的 `w4123.Dice*.dll` |
| `root_dir` | 空 | 骰子根目录，骰娘数据位于其下的 `Dice{QQ号}` 目录。为空时使用程序所在目录 |
| `workers` | `4` | 事件处理线程数。同一群聊或私聊的消息按接收顺序处理 |
| `rpc_timeout_ms` | `5000` | 向 NapCat 查询信息的超时时间（毫秒） |
| `list_cache_sec` | `600` | 好友列表、群列表的缓存时间（秒） |
| `member_cache_sec` | `300` | 群成员信息的缓存时间（秒） |
| `strip_reply` | `true` | 移除消息开头的回复引用，以便识别「回复 + @骰娘 + 指令」形式的消息 |
| `forward_poke` | `true` | 将戳一戳通知转为 Dice! 的 `Poke` 事件 |
| `forward_notice` | `false` | 将其他通知和请求转为 `OneBotNotice`、`OneBotRequest` 事件 |
| `media_base64` | `false` | 以 base64 方式发送图片、语音和文件。NapCat 与本程序不在同一台机器上时需要开启 |
| `log_level` | `info` | 日志级别：`debug`、`info`、`warn`、`error` |
| `log_to_file` | `true` | 是否将日志写入 `logs` 目录 |
| `restart_on_crash` | `true` | 核心异常退出后是否自动重启。重启间隔从 5 秒开始逐次加倍，最长 60 秒 |

Windows 下核心按系统代码页处理路径，`root_dir` 中不应包含当前代码页无法表示的字符。

## 扩展接口

`dice-host` 向 Dice! 模块提供以下扩展功能：

- 通过 Lua 函数 `askExtra` 调用 OneBot API；
- 将戳一戳通知转为 `Poke` 事件；
- 开启 `forward_notice` 后，将其他通知和请求转为 `OneBotNotice`、`OneBotRequest` 事件。

具体用法见 [docs/扩展接口.md](docs/扩展接口.md)。

## 核心补丁

发布包中的 Dice! 核心基于 newdev 分支编译，并应用了 `patches/dice-core` 中的三处修复，分别解决 MSVC 编译的核心在加载时卡死的问题、`AnysTable::from_json` 无限递归的问题，以及 Lua `askExtra` 传入 table 参数时参数丢失的问题。详细说明见 [patches/dice-core/README.md](patches/dice-core/README.md)。

## 已知问题

- NapCat 不提供 QQ 频道及讨论组消息，相关驱动接口未实现。
- 一个 `dice-host` 实例对应一个 QQ 账号。如需运行多个骰娘，请分别放置在不同目录中运行。

## 致谢

- [Dice!](https://github.com/Dice-Developer-Team/Dice)
- [NapCat](https://github.com/NapNeko/NapCatQQ)
- [nlohmann/json](https://github.com/nlohmann/json)

## License

NapDice

Copyright (C) 2026 NapDice contributors

This program is free software: you can redistribute it and/or modify it under the terms
of the GNU Affero General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along with this
program. If not, see <http://www.gnu.org/licenses/>.

`third_party/dice/DDAPI.h` 取自 Dice!，以 AGPL-3.0 授权；`third_party/nlohmann/json.hpp` 取自 nlohmann/json，以 MIT 授权。
