#!/usr/bin/env python3
"""
Contract test: the envelope-direct dispatch refactor is reflected in the
backend's observable signals.

Asserts:
  - GET /api/debug/signals exposes the direct-dispatch counters introduced by
    the refactor (ai_calls_inflight, ai_structured_submissions,
    ai_schema_validation_retries, ai_schema_validation_failures,
    ai_chunked_dispatches, ai_fence_strips).
  - The legacy SessionManager fields (session_managers_total / _active /
    _inflight / _completed / _failed) are NOT present — their removal was
    part of the refactor cleanup.
  - GET /api/status exposes `ai_calls_inflight` (the field that powers the
    dashboard LED) and does NOT expose `session_managers_*`.

Runs against a live JarvisAgent instance (default https://localhost:8443).
Requires an MCP admin key via --token or the J9T_TOKEN env var.

The debug endpoint requires a debug build (`make config=debug`); release
builds return a "not available" stub that this test reports as skipped.
"""

import argparse
import os
import sys
import urllib3

try:
    import requests
except ImportError:
    print("ERROR: requests package missing (pip install requests)")
    sys.exit(1)

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


EXPECTED_SIGNAL_FIELDS = [
    "ai_calls_inflight",
    "ai_structured_submissions",
    "ai_schema_validation_retries",
    "ai_schema_validation_failures",
    "ai_chunked_dispatches",
    "ai_fence_strips",
]

LEGACY_SIGNAL_FIELDS = [
    "session_managers_total",
    "session_managers_active",
    "session_managers_inflight",
    "session_managers_completed",
    "session_managers_failed",
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}

    # /api/status — public, no auth required, always present.
    rs = requests.get(f"{args.base_url}/api/status", verify=False, timeout=10)
    if rs.status_code != 200:
        print(f"FAIL: /api/status returned {rs.status_code}")
        return 1
    status = rs.json()
    if "ai_calls_inflight" not in status:
        print("FAIL: /api/status missing `ai_calls_inflight` field (dashboard LED signal)")
        return 1
    for legacy in LEGACY_SIGNAL_FIELDS:
        if legacy in status:
            print(f"FAIL: /api/status still exposes legacy field `{legacy}`")
            return 1
    print("OK: /api/status exposes ai_calls_inflight and no legacy session_managers_* fields")

    # /api/debug/signals — admin-auth, debug build only.
    rd = requests.get(
        f"{args.base_url}/api/debug/signals",
        headers=headers, verify=False, timeout=10,
    )
    if rd.status_code == 404:
        print("SKIP: /api/debug/signals not compiled in (release build); cannot verify new counters")
        print("OK (partial): status-endpoint invariants hold")
        return 0
    if rd.status_code != 200:
        print(f"FAIL: /api/debug/signals returned {rd.status_code}: {rd.text[:200]}")
        return 1
    body = rd.json()
    # Response shape: { "signals": { ... } }
    signals = body.get("signals", body)

    missing = [f for f in EXPECTED_SIGNAL_FIELDS if f not in signals]
    if missing:
        print(f"FAIL: /api/debug/signals missing refactor fields: {missing}")
        return 1

    leftover = [f for f in LEGACY_SIGNAL_FIELDS if f in signals]
    if leftover:
        print(f"FAIL: /api/debug/signals still exposes legacy session_managers fields: {leftover}")
        return 1

    print("OK: /api/debug/signals exposes all 6 direct-dispatch counters and no legacy fields")
    print(f"     ai_calls_inflight={signals['ai_calls_inflight']}, "
          f"ai_structured_submissions={signals['ai_structured_submissions']}, "
          f"ai_chunked_dispatches={signals['ai_chunked_dispatches']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
