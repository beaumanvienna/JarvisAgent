#!/usr/bin/env python3
"""
WebSocket payload verification for the ai-call-failed message extended in
Workstream B (m_ProviderErrorCode / m_ProviderErrorType / m_Category /
m_RetryAfterSeconds / m_InterfaceName).

Spins one `is_mock` interface per case, opens a WS subscriber, fires an
adhoc ai_call, and asserts the broadcast message carries the new fields.
Coverage spans all 4 production InterfaceTypes (Sitting 6 / Workstream E
expanded from 2 → 8 cases once cross-provider classification landed):

  api1 (OpenAI Chat)        billing_exhausted + throttle_rate_limit
  api3 (Google Gemini)      billing_exhausted + throttle_rate_limit
  api4 (Anthropic Messages) billing_exhausted + overload
  api5 (AWS Bedrock)        billing_exhausted + throttle_rate_limit

Each case asserts (category, provider_error_code, provider_error_type,
retry_after_seconds, interface_name, http_status) per the discriminator
shape of its provider — see CASES below and `test/dispatch/README.md`'s
WS payload verification section for the full table.

Runs against a live JarvisAgent instance (default https://localhost:8443).
Requires an MCP admin key via --token or the J9T_TOKEN env var, and the
websocket-client package.
"""

from __future__ import annotations

import argparse
import json
import os
import ssl
import sys
import threading
import time
from pathlib import Path

import urllib3

try:
    import requests
except ImportError:
    print("ERROR: requests package missing (pip install requests)")
    sys.exit(1)

try:
    import websocket as ws_client  # websocket-client
except ImportError:
    print("ERROR: websocket-client package missing (pip install websocket-client)")
    sys.exit(1)

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "test" / "dispatch"))

# Reuse the per-API helper for provisioning / trigger / cleanup so this driver
# only owns the WS subscription + assertions.
from _per_api_fault_helpers import (  # noqa: E402
    build_jcwf,
    create_mock_interface,
    delete_mock_interface,
    poll_run_state,
)


CASES = [
    # --- API1 OpenAI Chat (Workstream B / Sitting 4) ---
    {
        "name": "api1_billing_exhausted",
        "fixture_rel": "test/dispatch/fixtures/api1/error_billing.json",
        "api_type": "API1",
        "expected_category": "BillingExhausted",
        "expected_code": "insufficient_quota",
        "expected_type": "insufficient_quota",
        "expected_retry_after": None,           # no Retry-After header
        "expected_http_status": 429,
    },
    {
        "name": "api1_throttle_rate_limit",
        "fixture_rel": "test/dispatch/fixtures/api1/error_throttle.json",
        "api_type": "API1",
        "expected_category": "ThrottleRateLimit",
        # OpenAI's throttle envelope: type=discriminator, code=specific sub-reason
        # (see test/dispatch/fixtures/api1/error_throttle.json).  Both flow through.
        "expected_code": "rate_limit_exceeded",
        "expected_type": "rate_limit_error",
        "expected_retry_after": 12,             # Retry-After: 12 in meta.json
        "expected_http_status": 429,
    },
    # --- API3 Gemini (Workstream E / Sitting 6) ---
    # Gemini's billing vs throttle disambiguation comes from error.details[*].reason,
    # surfaced as m_ProviderErrorCode.  m_ProviderErrorType = status enum.
    {
        "name": "api3_billing_exhausted",
        "fixture_rel": "test/dispatch/fixtures/api3/error_billing.json",
        "api_type": "API3",
        "expected_category": "BillingExhausted",
        "expected_code": "USER_PROJECT_QUOTA_EXCEEDED",
        "expected_type": "RESOURCE_EXHAUSTED",
        "expected_retry_after": None,
        "expected_http_status": 429,
    },
    {
        "name": "api3_throttle_rate_limit",
        "fixture_rel": "test/dispatch/fixtures/api3/error_throttle.json",
        "api_type": "API3",
        "expected_category": "ThrottleRateLimit",
        "expected_code": "RATE_LIMIT_EXCEEDED",
        "expected_type": "RESOURCE_EXHAUSTED",
        "expected_retry_after": None,           # api3 throttle fixture has no Retry-After header
        "expected_http_status": 429,
    },
    # --- API4 Anthropic (Workstream E / Sitting 6) ---
    # Anthropic has no `code` field, only nested `error.type` (m_ProviderErrorCode stays empty).
    # credit_balance_too_low → BillingExhausted; HTTP 400 (Anthropic's non-429 billing quirk).
    {
        "name": "api4_billing_exhausted",
        "fixture_rel": "test/dispatch/fixtures/api4/error_billing.json",
        "api_type": "API4",
        "expected_category": "BillingExhausted",
        "expected_code": "",
        "expected_type": "credit_balance_too_low",
        "expected_retry_after": None,
        "expected_http_status": 400,
    },
    {
        "name": "api4_overload",
        "fixture_rel": "test/dispatch/fixtures/api4/error_overload.json",
        "api_type": "API4",
        "expected_category": "ServiceOverload",
        "expected_code": "",
        "expected_type": "overloaded_error",
        "expected_retry_after": 5,              # fixture's meta.json sets Retry-After: 5
        "expected_http_status": 529,            # Anthropic's non-standard overload status
    },
    # --- API5 Bedrock (Workstream E / Sitting 6) ---
    # AWS `__type` envelope pre-parsed in ReplyParserAPI5 ctor; m_ProviderErrorType = exception
    # short name (prefix-stripped if "com.amazonaws...#" present).  m_ProviderErrorCode stays
    # empty (AWS doesn't have a separate code field).
    {
        "name": "api5_billing_exhausted",
        "fixture_rel": "test/dispatch/fixtures/api5/error_billing.json",
        "api_type": "API5",
        "expected_category": "BillingExhausted",
        "expected_code": "",
        "expected_type": "ServiceQuotaExceededException",
        "expected_retry_after": None,
        "expected_http_status": 400,            # AWS uses 400 for client-side quota exhaustion
    },
    {
        "name": "api5_throttle_rate_limit",
        "fixture_rel": "test/dispatch/fixtures/api5/error_throttle.json",
        "api_type": "API5",
        "expected_category": "ThrottleRateLimit",
        "expected_code": "",
        "expected_type": "ThrottlingException",
        "expected_retry_after": None,
        "expected_http_status": 429,
    },
]


class WsSubscriber:
    """Background WS receiver — accumulates JSON messages of a chosen type.

    The j9t server flushes its pending-broadcast queue only inside the WS
    onmessage handler (webServer.cpp:3921), so a passive client never receives
    anything.  We send a small {"type":"ping"} every 300 ms to keep
    DrainPendingBroadcasts firing, matching the dashboard's heartbeat shape."""

    def __init__(self, url, token, sslopt):
        self._url = url
        self._token = token
        self._sslopt = sslopt
        self._messages = []
        self._lock = threading.Lock()
        self._ws = None
        self._thread = None
        self._ping_thread = None
        self._ready = threading.Event()
        self._stop = threading.Event()
        self._error = None

    def _on_open(self, _ws):
        self._ready.set()

    def _on_message(self, _ws, raw):
        try:
            payload = json.loads(raw)
        except Exception:
            return
        with self._lock:
            self._messages.append(payload)

    def _on_error(self, _ws, err):
        self._error = err
        self._ready.set()

    def _run(self):
        self._ws = ws_client.WebSocketApp(
            self._url,
            header=[f"Authorization: Bearer {self._token}"],
            on_open=self._on_open,
            on_message=self._on_message,
            on_error=self._on_error,
        )
        self._ws.run_forever(sslopt=self._sslopt, ping_interval=20)

    def _ping_loop(self):
        # Send a heartbeat every 300 ms to trigger server-side flush.  Stop on
        # close / shutdown.  Any exception (connection closed) ends the loop.
        while not self._stop.is_set():
            try:
                if self._ws is not None and self._ws.sock is not None:
                    self._ws.send(json.dumps({"type": "ping"}))
            except Exception:
                return
            self._stop.wait(0.3)

    def start(self, timeout_s=5):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        if not self._ready.wait(timeout=timeout_s):
            raise RuntimeError(f"WS did not become ready within {timeout_s}s")
        if self._error is not None:
            raise RuntimeError(f"WS error during handshake: {self._error!r}")
        self._ping_thread = threading.Thread(target=self._ping_loop, daemon=True)
        self._ping_thread.start()

    def stop(self):
        self._stop.set()
        if self._ws is not None:
            try:
                self._ws.close()
            except Exception:
                pass
        if self._ping_thread is not None:
            self._ping_thread.join(timeout=1)
        if self._thread is not None:
            self._thread.join(timeout=2)

    def wait_for(self, predicate, timeout_s=30):
        """Block until a message matching `predicate(msg) -> bool` arrives.
        Returns the matching message or None on timeout."""
        deadline = time.time() + timeout_s
        seen = 0
        while time.time() < deadline:
            with self._lock:
                snapshot = list(self._messages[seen:])
                seen = len(self._messages)
            for msg in snapshot:
                if predicate(msg):
                    return msg
            time.sleep(0.1)
        return None


def run_one_case(case, base_url, headers, token, sslopt, suffix):
    name = case["name"]
    fixture_path = REPO_ROOT / case["fixture_rel"]
    if not fixture_path.is_file():
        return False, f"[{name}] fixture not found: {fixture_path}"

    interface_name = f"mock_ws_{name}_{suffix}"
    ws_url = base_url.replace("http://", "ws://").replace("https://", "wss://") + "/ws"
    subscriber = WsSubscriber(ws_url, token, sslopt)

    if not create_mock_interface(base_url, headers, interface_name,
                                 case["api_type"], fixture_path):
        return False, f"[{name}] failed to provision mock interface"

    try:
        subscriber.start(timeout_s=5)
        # Trigger the adhoc ai_call.  api_label is just a JCWF naming hint —
        # build_jcwf uses it for the workflow id suffix; the actual dispatch
        # route is determined by the interface's `api_type`, set above.
        api_label = case["api_type"].lower()
        r = requests.post(f"{base_url}/api/workflows/run-adhoc",
                          json={"jcwf": build_jcwf(interface_name, suffix, api_label),
                                "cleanup_policy": "ttl_1h"},
                          headers=headers, verify=False, timeout=30)
        if r.status_code not in (200, 202):
            return False, f"[{name}] run-adhoc returned {r.status_code}: {r.text[:200]}"
        run_id = r.json().get("runId") or r.json().get("run_id")
        if not run_id:
            return False, f"[{name}] no runId in response"

        terminal = poll_run_state(base_url, headers, run_id, timeout_s=30)
        if terminal != "failed":
            return False, (f"[{name}] expected run state 'failed' but got '{terminal}'; "
                           f"runId={run_id}")

        # Find the matching ai-call-failed broadcast.  j9t wraps queued broadcasts
        # in {"type":"batch","messages":[...]} (webServer.cpp:DrainPendingBroadcasts)
        # so peek inside the inner array.  Multiple runs may be in flight (other
        # test suites); filter on interface_name to be sure.
        target_iface = interface_name

        def find_target(envelope):
            inner = envelope.get("messages") if envelope.get("type") == "batch" else [envelope]
            if not isinstance(inner, list):
                return None
            for m in inner:
                if (isinstance(m, dict)
                        and m.get("type") == "ai-call-failed"
                        and m.get("interface_name") == target_iface):
                    return m
            return None

        match_holder = {"msg": None}

        def predicate(envelope):
            found = find_target(envelope)
            if found is not None:
                match_holder["msg"] = found
                return True
            return False

        envelope = subscriber.wait_for(predicate, timeout_s=15)
        msg = match_holder["msg"]
        if envelope is None or msg is None:
            with subscriber._lock:
                recent = subscriber._messages[-10:]
            return False, (f"[{name}] no ai-call-failed WS message for interface "
                           f"'{target_iface}' within 15s; recent envelopes: {recent!r}")

        # Field-by-field assertions.
        problems = []
        if msg.get("category") != case["expected_category"]:
            problems.append(f"category={msg.get('category')!r} "
                            f"(want {case['expected_category']!r})")
        if msg.get("provider_error_code") != case["expected_code"]:
            problems.append(f"provider_error_code={msg.get('provider_error_code')!r} "
                            f"(want {case['expected_code']!r})")
        if msg.get("provider_error_type") != case["expected_type"]:
            problems.append(f"provider_error_type={msg.get('provider_error_type')!r} "
                            f"(want {case['expected_type']!r})")
        if msg.get("interface_name") != interface_name:
            problems.append(f"interface_name={msg.get('interface_name')!r} "
                            f"(want {interface_name!r})")
        if msg.get("http_status") != case["expected_http_status"]:
            problems.append(f"http_status={msg.get('http_status')!r} "
                            f"(want {case['expected_http_status']!r})")
        if case["expected_retry_after"] is None:
            if "retry_after_seconds" in msg:
                problems.append(
                    f"retry_after_seconds={msg.get('retry_after_seconds')!r} "
                    f"(want field absent)")
        else:
            if msg.get("retry_after_seconds") != case["expected_retry_after"]:
                problems.append(
                    f"retry_after_seconds={msg.get('retry_after_seconds')!r} "
                    f"(want {case['expected_retry_after']!r})")

        if problems:
            return False, f"[{name}] WS payload mismatch: " + "; ".join(problems) \
                          + f" | full msg: {json.dumps(msg)}"

        return True, f"[{name}] OK (runId={run_id}, category={msg.get('category')})"

    finally:
        subscriber.stop()
        delete_mock_interface(base_url, headers, interface_name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}
    sslopt = {"cert_reqs": ssl.CERT_NONE} if args.base_url.startswith("https://localhost") else {}
    suffix = str(int(time.time()))

    results = []
    for case in CASES:
        ok, msg = run_one_case(case, args.base_url, headers, args.token, sslopt, suffix)
        print(msg if ok else f"FAIL {msg}")
        results.append(ok)

    total = len(results)
    passed = sum(1 for x in results if x)
    print(f"\n== test_ws_ai_call_failed_payload: {passed}/{total} pass ==")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
