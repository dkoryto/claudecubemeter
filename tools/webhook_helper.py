#!/usr/bin/env python3
"""Webhook helper for Clawdmeter shortcuts on the GeekMagic SmallTV-Ultra port.

The device cannot inject keystrokes directly (no BLE/USB HID on ESP8266).
Instead it POSTs {"key":"voice"|"toggle"} to a URL of your choosing.
This script listens for those POSTs and types the keystroke on the local
machine via `osascript` (macOS) or `xdotool` (Linux).

Usage:
    python3 webhook_helper.py --port 8765

Then in the device's web UI set the webhook URL to
    http://<this-machine-ip>:8765/
"""

import argparse
import json
import platform
import shutil
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


KEY_VOICE = "voice"
KEY_TOGGLE = "toggle"


def type_keystroke(key: str) -> tuple[bool, str]:
    system = platform.system()
    if system == "Darwin":
        return _type_macos(key)
    if system == "Linux":
        return _type_linux(key)
    return False, f"unsupported platform: {system}"


def _type_macos(key: str) -> tuple[bool, str]:
    if key == KEY_VOICE:
        script = 'tell application "System Events" to keystroke " "'
    elif key == KEY_TOGGLE:
        # key code 48 = Tab; shift down = Shift+Tab
        script = 'tell application "System Events" to key code 48 using {shift down}'
    else:
        return False, f"unknown key: {key}"
    try:
        subprocess.run(["osascript", "-e", script], check=True, timeout=5)
        return True, "ok"
    except subprocess.CalledProcessError as e:
        return False, f"osascript failed: {e}"


def _type_linux(key: str) -> tuple[bool, str]:
    if shutil.which("xdotool") is None:
        return False, "xdotool not installed (apt install xdotool)"
    if key == KEY_VOICE:
        cmd = ["xdotool", "key", "space"]
    elif key == KEY_TOGGLE:
        cmd = ["xdotool", "key", "shift+Tab"]
    else:
        return False, f"unknown key: {key}"
    try:
        subprocess.run(cmd, check=True, timeout=5)
        return True, "ok"
    except subprocess.CalledProcessError as e:
        return False, f"xdotool failed: {e}"


class Handler(BaseHTTPRequestHandler):
    def _reply(self, code: int, payload: dict) -> None:
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""
        try:
            data = json.loads(raw.decode() or "{}")
        except json.JSONDecodeError:
            self._reply(400, {"status": "error", "message": "invalid json"})
            return
        key = data.get("key", "")
        if not key:
            self._reply(400, {"status": "error", "message": "missing key"})
            return
        ok, msg = type_keystroke(key)
        self._reply(200 if ok else 500, {"status": "ok" if ok else "error", "message": msg})

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stderr.write(f"[{self.log_date_time_string()}] {fmt % args}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", default=8765, type=int)
    args = parser.parse_args()

    server = HTTPServer((args.host, args.port), Handler)
    print(f"Listening on http://{args.host}:{args.port}/ — POST {{'key':'voice'|'toggle'}}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
