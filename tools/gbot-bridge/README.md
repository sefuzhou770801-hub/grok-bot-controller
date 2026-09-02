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

代理不读写 token；gbot CLI 自己使用本机 Grok Bot 应用的登录态。

## 依赖

- **Python 3.10+**（标准库即可，无需 pip 包）
- **gbot CLI**（外部工具，不在本仓库）：[`grok-bot-cli`](https://www.npmjs.com/package/grok-bot-cli)
- **zsh**（仅 `crab-demo` 需要）
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

### 异常输入与并发

请求体是合法但非对象的 JSON（数字、字符串、数组）时返回 HTTP 400，不断开连接：

```sh
curl -sS -D - -o - -X POST "http://127.0.0.1:18770/send" \
  -H "Content-Type: application/json" \
  -d '42'
```

应看到 `HTTP/1.0 400` 与 `{"ok": false, "error": "bad json"}`。`"text"` 与 `[1]` 同理。

流式请求在写出 NDJSON 之前失败（例如 `gbot thread` 认证失败）时，返回带状态行的 HTTP 502，而不是无头的一行 JSON。用会失败的假 `gbot` 复现：

```sh
cat > /tmp/fail-gbot <<'SH'
#!/bin/sh
echo "not authenticated" >&2
exit 1
SH
chmod +x /tmp/fail-gbot
STACKCHAN_GBOT_BIN=/tmp/fail-gbot STACKCHAN_GBOT_HTTP_PORT=18771 \
  ./tools/gbot-bridge/run_gbot_http.sh
```

另开终端：

```sh
curl -sS -D - -o - -X POST "http://127.0.0.1:18771/send" \
  -H "Content-Type: application/json" \
  -H "X-Stackchan-Stream: 1" \
  -d '{"text":"ping"}'
```

第一行必须是 `HTTP/1.0 502`（或 `HTTP/1.1 502`），正文为 `ok: false` 的 JSON 对象。

同一 bot 的两个 `/send` 并发时按 bot 串行处理，各自拿到自己的回复。用仓库里的假 gbot 做 curl 复现：

```sh
STATE=/tmp/fake-gbot-state.json
ROOT="$(pwd)"
echo '{"entries": [], "seq": 0}' > "$STATE"
cat > /tmp/ok-gbot <<SH
#!/bin/sh
exec python3 "$ROOT/tools/gbot-bridge/test/fake_gbot.py" "\$@"
SH
chmod +x /tmp/ok-gbot
FAKE_GBOT_STATE="$STATE" FAKE_GBOT_THREAD_DELAY_S=0.30 \
  STACKCHAN_GBOT_BIN=/tmp/ok-gbot STACKCHAN_GBOT_HTTP_PORT=18771 \
  ./tools/gbot-bridge/run_gbot_http.sh
```

另开终端，两条请求同时发出：

```sh
curl -sS -X POST "http://127.0.0.1:18771/send" \
  -H "Content-Type: application/json" -d '{"text":"alpha"}' &
curl -sS -X POST "http://127.0.0.1:18771/send" \
  -H "Content-Type: application/json" -d '{"text":"beta"}' &
wait
```

两条响应应分别包含 `"reply": "reply-alpha。"` 与 `"reply": "reply-beta。"`，不能两条都是同一句。

自动化测试（含上述三场景与 `/health`、`/send` 正常路径）：

```sh
python3 -m unittest discover -s tools/gbot-bridge/test -v
```

## crab-demo 用法

设备需已在同一局域网，且固件开了 `/mcp/expression` 与 `/mcp/balloon`（设置页配置 MCP token）。本脚本不启动 HTTP 代理，也不调用 gbot。

```sh
CRAB_HOST=<设备局域网地址> CRAB_TOKEN=<MCP token> \
  ./tools/gbot-bridge/crab-demo "帮我看看这个仓库里有几个 TODO"
```

未设置 `CRAB_HOST` / `CRAB_TOKEN`、或 `grok` 不在 PATH 中时，脚本会报错退出。真机表情与气泡效果由维护者在设备上验收。
