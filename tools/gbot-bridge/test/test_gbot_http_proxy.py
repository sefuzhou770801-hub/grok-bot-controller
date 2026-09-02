# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""gbot HTTP 代理的公开接口测试：POST /send 与 GET /health。"""

from __future__ import annotations

import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROXY = HERE.parent / "gbot_http_proxy.py"
FAKE_GBOT = HERE / "fake_gbot.py"


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class ProxyServer:
    def __init__(self, tmp: Path, extra_env: dict[str, str] | None = None) -> None:
        self.port = _free_port()
        self.state = tmp / "fake-gbot-state.json"
        self.state.write_text('{"entries": [], "seq": 0}', encoding="utf-8")
        wrapper = tmp / "gbot"
        wrapper.write_text(
            "#!/bin/sh\n"
            f'exec {sys.executable} "{FAKE_GBOT}" "$@"\n',
            encoding="utf-8",
        )
        wrapper.chmod(0o755)
        env = os.environ.copy()
        env.update(
            {
                "STACKCHAN_GBOT_BIN": str(wrapper),
                "STACKCHAN_GBOT_HTTP_PORT": str(self.port),
                "STACKCHAN_ASK_BOT": "总管",
                "STACKCHAN_GBOT_POLL_S": "0.05",
                "STACKCHAN_GBOT_STABLE_S": "0.05",
                "STACKCHAN_GBOT_FIRST_TIMEOUT": "5",
                "STACKCHAN_GBOT_FOLLOW_S": "5",
                "FAKE_GBOT_STATE": str(self.state),
                "PYTHONUNBUFFERED": "1",
            }
        )
        if extra_env:
            env.update(extra_env)
        self.proc = subprocess.Popen(
            [sys.executable, str(PROXY)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def wait_ready(self, timeout_s: float = 5.0) -> None:
        deadline = time.monotonic() + timeout_s
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                stdout, stderr = self.proc.communicate(timeout=1)
                raise RuntimeError(
                    "proxy exited early "
                    f"code={self.proc.returncode} stdout={stdout.decode()} stderr={stderr.decode()}"
                )
            try:
                self.request("GET", "/health")
                return
            except Exception as exc:  # noqa: BLE001 — 启动期连接拒绝是预期
                last_error = exc
                time.sleep(0.05)
        raise TimeoutError(f"proxy did not become ready: {last_error}")

    def request(
        self,
        method: str,
        path: str,
        *,
        body: bytes | None = None,
        headers: dict[str, str] | None = None,
    ) -> tuple[int, dict]:
        req_headers = {"Content-Type": "application/json"}
        if headers:
            req_headers.update(headers)
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}",
            data=body,
            method=method,
            headers=req_headers,
        )
        try:
            with urllib.request.urlopen(req, timeout=8) as resp:
                raw = resp.read()
                payload = json.loads(raw.decode("utf-8")) if raw else {}
                return resp.status, payload
        except urllib.error.HTTPError as exc:
            try:
                raw = exc.read()
                payload = json.loads(raw.decode("utf-8")) if raw else {}
                return exc.code, payload
            finally:
                exc.close()

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=3)


class SendRejectsNonObjectJson(unittest.TestCase):
    def _assert_bad_json(self, body: bytes) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            server = ProxyServer(Path(tmp))
            try:
                server.wait_ready()
                code, payload = server.request("POST", "/send", body=body)
                self.assertEqual(code, 400)
                self.assertEqual(payload.get("ok"), False)
                self.assertEqual(payload.get("error"), "bad json")
            finally:
                server.close()

    def test_number_body_returns_400_json(self) -> None:
        self._assert_bad_json(b"42")

    def test_string_body_returns_400_json(self) -> None:
        self._assert_bad_json(b'"text"')

    def test_array_body_returns_400_json(self) -> None:
        self._assert_bad_json(b"[1]")


class StreamFailureSendsHttpStatus(unittest.TestCase):
    def test_thread_auth_failure_returns_502_before_ndjson(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            server = ProxyServer(Path(tmp), extra_env={"FAKE_GBOT_THREAD_FAIL": "1"})
            try:
                server.wait_ready()
                try:
                    code, payload = server.request(
                        "POST",
                        "/send",
                        body=b'{"text":"ping"}',
                        headers={"X-Stackchan-Stream": "1"},
                    )
                except (http.client.BadStatusLine, http.client.RemoteDisconnected) as exc:
                    self.fail(f"streaming failure wrote a body without HTTP headers: {exc}")
                self.assertEqual(code, 502)
                self.assertEqual(payload.get("ok"), False)
                self.assertTrue(str(payload.get("error") or ""))
            finally:
                server.close()


class ConcurrentSameBotSend(unittest.TestCase):
    def test_overlapping_sends_return_matching_replies(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            server = ProxyServer(
                Path(tmp),
                extra_env={"FAKE_GBOT_THREAD_DELAY_S": "0.30"},
            )
            try:
                server.wait_ready()
                results: dict[str, tuple[int, dict]] = {}
                errors: dict[str, Exception] = {}

                def post(name: str) -> None:
                    try:
                        results[name] = server.request(
                            "POST",
                            "/send",
                            body=json.dumps({"text": name}, ensure_ascii=False).encode("utf-8"),
                        )
                    except Exception as exc:  # noqa: BLE001 — 收集线程内异常到主线程断言
                        errors[name] = exc

                workers = [
                    threading.Thread(target=post, args=("alpha",)),
                    threading.Thread(target=post, args=("beta",)),
                ]
                for worker in workers:
                    worker.start()
                for worker in workers:
                    worker.join(timeout=10)
                    self.assertFalse(worker.is_alive(), "request thread did not finish")

                self.assertEqual(errors, {})
                code_a, payload_a = results["alpha"]
                code_b, payload_b = results["beta"]
                self.assertEqual(code_a, 200)
                self.assertEqual(code_b, 200)
                self.assertEqual(payload_a.get("ok"), True)
                self.assertEqual(payload_b.get("ok"), True)
                self.assertEqual(payload_a.get("reply"), "reply-alpha。")
                self.assertEqual(payload_b.get("reply"), "reply-beta。")
            finally:
                server.close()


class HealthAndSendHappyPath(unittest.TestCase):
    def test_health_reports_ok(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            server = ProxyServer(Path(tmp))
            try:
                server.wait_ready()
                code, payload = server.request("GET", "/health")
                self.assertEqual(code, 200)
                self.assertEqual(payload.get("ok"), True)
                self.assertEqual(payload.get("service"), "gbot-http")
                self.assertEqual(payload.get("listen"), server.port)
            finally:
                server.close()

    def test_send_returns_fake_gbot_reply(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            server = ProxyServer(Path(tmp))
            try:
                server.wait_ready()
                code, payload = server.request(
                    "POST",
                    "/send",
                    body=b'{"text":"ping"}',
                )
                self.assertEqual(code, 200)
                self.assertEqual(payload.get("ok"), True)
                self.assertEqual(payload.get("reply"), "reply-ping。")
            finally:
                server.close()


if __name__ == "__main__":
    unittest.main()
