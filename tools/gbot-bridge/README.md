<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# gbot-bridge

让 Groki Bot 接上 Grok Bot 对话的本机联动层。克隆本仓库后按下面步骤准备外部依赖即可启动。

本目录包含：

| 文件 | 作用 |
|---|---|
| `gbot_http_proxy.py` | 本机 HTTP 代理：把 `POST /send` 转成 `gbot --json send`，轮询 thread 取回复 |
| `run_gbot_http.sh` | 启动脚本（强制只绑 `127.0.0.1`） |
| `stackchan_mcp/ask.py` | ask 客户端（仅 Python 标准库）：解析 gbot、发送、读取 thread |
| `crab-demo` | 演示脚本：派任务给 grok CLI，设备用思考/兴奋/疑惑脸外化状态 |
| `groki-face` | 拍摄遥控：按键或预设段切换表情、发送气泡 |

代理不读写 token；gbot CLI 自己使用本机 Grok Bot 应用的登录态。

## 依赖

- **Python 3.10+**（标准库即可，无需 pip 包）
- **gbot CLI**（外部工具，不在本仓库）：[`grok-bot-cli`](https://www.npmjs.com/package/grok-bot-cli)
- **zsh**（`crab-demo`、`groki-face`）
- **curl / dns-sd**（仅 `groki-face`，macOS 自带）
- **grok CLI**（仅 `crab-demo` 需要，与 gbot 不是同一个命令）
- 运行代理的机器上已打开并登录 Grok Bot 应用

### 安装 gbot CLI

```sh
npm install --global grok-bot-cli
command -v gbot
gbot bots list
```

需要 Node.js 18+。若 `gbot` 不在 PATH 中，把可执行文件的绝对路径写到 `STACKCHAN_GBOT_BIN`。

`gbot` 默认读取本机 Grok Bot 应用的会话，不必把 token 配给本代理。认证方式以 `grok-bot-cli` 文档为准。

默认 bot 名是 `总管`。若你的 Grok Bot 里没有这个名字，先 `gbot bots create --name 总管`，或设置 `STACKCHAN_ASK_BOT` 指向已有 bot。

## 环境变量

| 变量 | 默认 | 用途 |
|---|---|---|
| `STACKCHAN_GBOT_BIN` | PATH 中的 `gbot` | gbot 可执行文件路径 |
| `STACKCHAN_ASK_GBOT` | （未设） | `STACKCHAN_GBOT_BIN` 的兼容旧名 |
| `STACKCHAN_ASK_BOT` | `总管` | 默认 bot 名称 |
| `STACKCHAN_GBOT_HTTP_HOST` | `127.0.0.1` | 保留此变量名。代理始终只绑回环，写成其他地址无效 |
| `STACKCHAN_GBOT_HTTP_PORT` | `18770` | 代理监听端口 |
| `STACKCHAN_GBOT_POLL_S` | `0.10` | thread 轮询间隔（秒） |
| `STACKCHAN_GBOT_STABLE_S` | `0.35` | 无句号短句判定为停笔的稳定时间（秒） |
| `STACKCHAN_GBOT_FIRST_TIMEOUT` | `30` | 等待首句的超时（秒） |
| `STACKCHAN_GBOT_FOLLOW_S` | `110` | 流式跟随的硬超时（秒） |
| `STACKCHAN_GBOT_FOLLOW_IDLE_S` | `90` | 流式空闲结束（秒） |
| `STACKCHAN_GBOT_FOLLOW_BUSY_IDLE_S` | `100` | 看起来还在干活时的空闲结束（秒） |
| `STACKCHAN_GBOT_FOLLOW_MAX` | `4` | 流式后续条数上限 |
| `CRAB_HOST` | （必填） | 设备 HTTP 地址，不含协议。仅 `crab-demo` |
| `CRAB_TOKEN` | （必填） | 设备设置页的 MCP token。仅 `crab-demo` |
| `CRAB_CWD` | 当前目录 | grok 任务工作目录。仅 `crab-demo` |
| `GROKI_HOST` | 自动发现 | 设备地址，不含协议（如 `stackchan-XXXXXX.local`）。未设时用 dns-sd 查找第一个 `stackchan-*.local`，超时 3 秒。仅 `groki-face` |
| `GROKI_TOKEN` | （必填） | 设备设置页的 MCP token。仅 `groki-face` |

## 启动与健康检查

在仓库根目录：

```sh
./tools/gbot-bridge/run_gbot_http.sh
```

另开一个终端：

```sh
curl -sS "http://127.0.0.1:18770/health"
```

成功时应得到 JSON：`{"ok": true, "service": "gbot-http", "listen": 18770}`。

未安装 gbot 时，健康检查仍然成功（代理进程本身活着）。发消息会得到明确错误而不是崩溃：

```sh
curl -sS -X POST "http://127.0.0.1:18770/send" \
  -H "Content-Type: application/json" \
  -d '{"text":"ping"}'
```

此时 JSON 里 `ok` 为 false，`error` 提示找不到 gbot。装好 gbot 并登录 Grok Bot 之后，同一请求会把文本发给默认 bot，并在首句就绪后返回 `ok: true` 与 `reply`。

## crab-demo 用法

设备需已在同一局域网，且固件开了 `/mcp/expression` 与 `/mcp/balloon`（设置页配置 MCP token）。本脚本不启动 HTTP 代理，也不调用 gbot。

```sh
CRAB_HOST=<设备局域网地址> CRAB_TOKEN=<MCP token> \
  ./tools/gbot-bridge/crab-demo "帮我看看这个仓库里有几个 TODO"
```

未设置 `CRAB_HOST` / `CRAB_TOKEN`、或 `grok` 不在 PATH 中时，脚本会报错退出。真机表情与气泡效果由维护者在设备上验收。

## 拍摄遥控 groki-face

拍摄时按一个键切换表情，或按预设段自动串演。只依赖 zsh、curl、dns-sd（macOS 自带），不改固件。

设备需已在同一局域网，设置页已配置 MCP token。脚本启动时先 `GET /mcp/state` 做连通检查，成功打印设备 IP 与固件版本。

### 配置

环境变量优先；未设时读 `~/.config/groki/env`（`KEY=VALUE` 每行一条，该文件不入库）：

```
GROKI_HOST=stackchan-XXXXXX.local
GROKI_TOKEN=<MCP_TOKEN>
```

`GROKI_HOST` 可省略：脚本用 `dns-sd -B _stackchan-config._tcp local`（找不到再试 `_http._tcp`）发现第一个 `stackchan-*.local`，超时 3 秒。`GROKI_TOKEN` 必填。

### 用法

```sh
# 单次切表情
GROKI_HOST=<设备地址> GROKI_TOKEN=<MCP_TOKEN> \
  ./tools/gbot-bridge/groki-face happy

# 交互式按键（q 退出）
GROKI_HOST=<设备地址> GROKI_TOKEN=<MCP_TOKEN> \
  ./tools/gbot-bridge/groki-face

# 按预设段串演（内置 demo）
GROKI_HOST=<设备地址> GROKI_TOKEN=<MCP_TOKEN> \
  ./tools/gbot-bridge/groki-face play demo

# 发送气泡
GROKI_HOST=<设备地址> GROKI_TOKEN=<MCP_TOKEN> \
  ./tools/gbot-bridge/groki-face balloon "<文字>" 3000
```

已写入 `~/.config/groki/env` 时，可直接 `./tools/gbot-bridge/groki-face`。

### 按键表

| 键 | 表情 | 键 | 表情 |
|---|---|---|---|
| `1` | neutral | `2` | happy |
| `3` | sad | `4` | angry |
| `5` | doubt | `6` | sleepy |
| `7` | listening | `8` | thinking |
| `9` | excited | `0` | curious |
| `c` | confused | `s` | surprised |
| `d` | dizzy | `a` | affection |
| `b` | bored | `i` | idle |
| `q` | 退出 | | |

气泡不走按键，用 `groki-face balloon` 单独发。表情请求非 200 时终端打一行红色提示，交互模式不中断。`curl` 超时 3 秒。

### demo 段

sleepy 3 秒 → surprised 1 秒 → happy 2 秒 → listening 2 秒 → thinking 2 秒 → excited 2 秒 → happy / angry / sad / confused / dizzy 各 0.6 秒 → affection 3 秒 → bored 2 秒 → sleepy（停在此）。

新增段：在脚本内的 `GROKI_SEGMENTS` 表加一项即可。
