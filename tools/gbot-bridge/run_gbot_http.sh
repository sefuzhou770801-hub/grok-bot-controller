#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
export STACKCHAN_GBOT_HTTP_PORT="${STACKCHAN_GBOT_HTTP_PORT:-18770}"
export STACKCHAN_ASK_BOT="${STACKCHAN_ASK_BOT:-总管}"
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"
exec python3 "$DIR/gbot_http_proxy.py"
