#!/usr/bin/env python3
"""Claude usage proxy for the SmallTV-Ultra Clawdmeter.

Polls api.anthropic.com rate-limit headers every 60 s, pushes the parsed
values to the device's /api/v1/clawd/data endpoint over plain HTTP. Lives
on a host machine because the ESP8266 cannot fit a 16 KB TLS RX buffer
(Anthropic's CDN does not negotiate MFLN).

Usage:
    python3 claude_proxy.py --device 192.168.10.43 --bearer clawd-2026 \
        --token sk-ant-oat01-...

or use environment variables CLAWD_DEVICE / CLAWD_BEARER / ANTHROPIC_TOKEN.

The token can also be auto-read from ~/.claude/.credentials.json on Linux
or the macOS Keychain entry "Claude Code-credentials" (the accessToken
field). Pass --auto-token to enable this.
"""

import argparse
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ANTHROPIC_URL = "https://api.anthropic.com/v1/messages"
ANTHROPIC_BODY = json.dumps(
    {
        "model": "claude-haiku-4-5-20251001",
        "max_tokens": 1,
        "messages": [{"role": "user", "content": "hi"}],
    }
).encode()
POLL_INTERVAL = 60.0
REQUEST_TIMEOUT = 20.0

KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def extract_access_token(blob: str) -> str | None:
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE,
             "-a", getpass.getuser(), "-w"],
            check=True, capture_output=True, text=True, timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed: {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return extract_access_token(out.stdout)


def read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Credentials file unreadable: {e}")
        return None
    return extract_access_token(raw)


def auto_read_token() -> str | None:
    if sys.platform == "darwin":
        return read_token_keychain()
    return read_token_file()


def poll_anthropic(token: str) -> dict | None:
    req = urllib.request.Request(
        ANTHROPIC_URL,
        data=ANTHROPIC_BODY,
        method="POST",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",
            "User-Agent": "claude-code/2.1.5",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT) as resp:
            headers = dict(resp.headers.items())
    except urllib.error.HTTPError as e:
        log(f"Anthropic HTTP {e.code}: {e.read()[:200]!r}")
        # Even error responses carry rate-limit headers; try to parse them.
        headers = dict(e.headers.items()) if e.headers else {}
        if not headers:
            return None
    except urllib.error.URLError as e:
        log(f"Anthropic network error: {e.reason}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        # urllib normalizes header names lowercased on retrieval via items(),
        # but check both cases to be safe.
        return headers.get(name) or headers.get(name.lower()) or default

    now = time.time()

    def reset_min(ts: str) -> int:
        try:
            r = float(ts)
        except ValueError:
            return 0
        m = (r - now) / 60.0
        return int(round(m)) if m > 0 else 0

    def pct(u: str) -> int:
        try:
            v = int(round(float(u) * 100))
            return max(0, min(100, v))
        except ValueError:
            return 0

    s_util = hdr("anthropic-ratelimit-unified-5h-utilization")
    w_util = hdr("anthropic-ratelimit-unified-7d-utilization")
    if s_util == "0" and w_util == "0":
        log("No rate-limit headers in response")
        return None

    return {
        "session_pct": pct(s_util),
        "session_reset_min": reset_min(hdr("anthropic-ratelimit-unified-5h-reset")),
        "weekly_pct": pct(w_util),
        "weekly_reset_min": reset_min(hdr("anthropic-ratelimit-unified-7d-reset")),
        "status": hdr("anthropic-ratelimit-unified-5h-status", "allowed"),
    }


def push_to_device(device: str, bearer: str, payload: dict) -> bool:
    url = f"http://{device}/api/v1/clawd/data"
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode(),
        method="POST",
        headers={
            "Authorization": f"Bearer {bearer}",
            "Content-Type": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            if resp.status == 200:
                return True
            log(f"Device returned HTTP {resp.status}")
    except urllib.error.HTTPError as e:
        log(f"Device push HTTP {e.code}: {e.read()[:100]!r}")
    except urllib.error.URLError as e:
        log(f"Device push network error: {e.reason}")
    return False


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--device", default=os.environ.get("CLAWD_DEVICE", ""),
                   help="device IP or host (e.g. 192.168.10.43)")
    p.add_argument("--bearer", default=os.environ.get("CLAWD_BEARER", ""),
                   help="device bearer token (the GeekMagic api_token)")
    p.add_argument("--token", default=os.environ.get("ANTHROPIC_TOKEN", ""),
                   help="Anthropic OAuth access token; if omitted with --auto-token, "
                        "read from Keychain (macOS) or .credentials.json (Linux)")
    p.add_argument("--auto-token", action="store_true",
                   help="read Anthropic token from local Claude Code creds")
    p.add_argument("--interval", type=float, default=POLL_INTERVAL,
                   help=f"poll interval in seconds (default {POLL_INTERVAL})")
    args = p.parse_args()

    if not args.device:
        p.error("--device required (or CLAWD_DEVICE env)")
    if not args.bearer:
        p.error("--bearer required (or CLAWD_BEARER env)")

    stop = False

    def _stop(*_a: object) -> None:
        nonlocal stop
        log("Stopping")
        stop = True

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    log(f"Polling Anthropic every {args.interval:.0f}s → http://{args.device}/api/v1/clawd/data")

    while not stop:
        token = args.token or (auto_read_token() if args.auto_token else "")
        if not token:
            log("No Anthropic token (pass --token or --auto-token)")
            time.sleep(args.interval)
            continue
        payload = poll_anthropic(token)
        if payload is not None:
            log(f"Got payload: session={payload['session_pct']}% "
                f"weekly={payload['weekly_pct']}% status={payload['status']}")
            if push_to_device(args.device, args.bearer, payload):
                log("Pushed to device")
        # sleep with periodic stop check
        end = time.time() + args.interval
        while not stop and time.time() < end:
            time.sleep(0.5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
