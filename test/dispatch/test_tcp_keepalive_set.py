#!/usr/bin/env python3
"""
§14 Tier B hermetic test — TCP keepalive policy.

Asserts that CurlMultiDispatcher::SetupEasyHandle sets CURLOPT_TCP_KEEPALIVE = 1
on every easy handle (per `AI call performance optimization.md` §6.4).  The
flag is surfaced as `dispatcher_keepalive_enabled` in /api/debug/signals so
this test can verify the policy without poking at libcurl internals.

Zero network calls outside localhost.  Runs against a Studio Debug build on
the default port (8443).  Requires an admin MCP key via --token or J9T_TOKEN.
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}
    r = requests.get(f"{args.base_url}/api/debug/signals",
                     headers=headers, verify=False, timeout=10)
    if r.status_code != 200:
        print(f"FAIL: /api/debug/signals returned {r.status_code}: {r.text[:200]}")
        return 1

    body = r.json()
    if not body.get("ok"):
        print(f"FAIL: response missing ok=true: {body}")
        return 1

    signals = body.get("signals", {})
    enabled = signals.get("dispatcher_keepalive_enabled")
    if enabled is not True:
        print(f"FAIL: dispatcher_keepalive_enabled is {enabled!r} (expected True)")
        return 1

    print("PASS: dispatcher_keepalive_enabled = True")
    return 0


if __name__ == "__main__":
    sys.exit(main())
