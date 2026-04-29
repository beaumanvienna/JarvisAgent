#!/usr/bin/env python3
"""
§14 Tier B hermetic test — AIMD concurrency-cap halving on 429.

Asserts the multiplicative-decrease branch of `RateLimitController::Observe`
(rateLimitController.cpp:146-150): every 429 response halves the controller's
m_CurrentConcurrencyCap (floor 1) and resets the streak counter.

Drives a sequence of forced 429 responses through the localhost mock endpoint
and reads /api/debug/signals dispatcher_controllers[] after each one to verify
the cap halves on each 429 and stops at 1.

Runs against a Studio Debug build on the default port (8443).  Requires an
admin MCP key via --token or J9T_TOKEN.  Cleans up the provider + interface
on exit (best-effort).
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _tier_b_helpers as h  # noqa: E402

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


PROVIDER_NAME  = "tier_b_aimd_provider"
INTERFACE_NAME = "tier_b_aimd_iface"

# Anthropic strategy's InitialConcurrencyProbe = 4 (rateLimitStrategy.cpp:358).
# Disable retries so each Submit produces exactly one Observe(was_429=true)
# rather than a torrent of internal retries muddying the cap-halve count.
RATE_LIMIT = {
    "initial_concurrency_probe": 4,   # cap starts at 4 → halves: 4→2→1→1...
    "max_concurrency": 16,
    "max_retries_429": 0,             # no retries on 429
    "max_retries_transient": 0,
    "base_retry_ms": 100,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    args = parser.parse_args()
    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}
    h.reset_dispatcher_state(args.base_url, headers)

    if not h.provision_provider(args.base_url, headers, PROVIDER_NAME, api_type="API4"):
        return 1

    mock_url = h.mock_endpoint_url(args.base_url, status=429,
                                    header_fixture="anthropic_429",
                                    body_fixture="anthropic_429_error")
    if not h.provision_interface(args.base_url, headers,
                                  name=INTERFACE_NAME, api_type="API4",
                                  model="claude-sonnet-4-6", mock_url=mock_url,
                                  key_name=PROVIDER_NAME, rate_limit=RATE_LIMIT):
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)
        return 1

    failures = []
    try:
        # AIMD: cap=4 → 2 → 1 → 1 → 1 over four 429s (floor at 1).
        # Each Submit causes exactly one Observe(was_429=true) since retries
        # are disabled by RATE_LIMIT.max_retries_429=0.
        expected_caps = [2, 1, 1, 1]
        observed_caps = []

        for i in range(len(expected_caps)):
            jcwf = h.build_adhoc_jcwf(workflow_id=f"tier_b_aimd_{i}",
                                       interface_name=INTERFACE_NAME,
                                       prob_text=f"AIMD halving probe {i}.")
            run_id = h.submit_adhoc(args.base_url, headers, jcwf)
            if not run_id:
                failures.append(f"step {i}: run-adhoc failed")
                continue
            state = h.poll_run_state(args.base_url, headers, run_id, timeout_s=15)

            signals = h.get_signals(args.base_url, headers)
            ctrl = h.find_controller(signals, "sonnet")
            if ctrl is None:
                # Try matching by host substring as a fallback.
                ctrl = h.find_controller(signals, "localhost")
            if ctrl is None:
                failures.append(f"step {i}: controller not found in signals")
                observed_caps.append(None)
                continue

            cap = ctrl["current_concurrency_cap"]
            streak = ctrl["streak_since_last_429"]
            observed_caps.append(cap)
            print(f"  step {i+1}/{len(expected_caps)}: run={run_id} state={state} "
                  f"cap={cap} streak={streak} expected_cap={expected_caps[i]}")

            if cap != expected_caps[i]:
                failures.append(f"step {i+1}: cap={cap} expected={expected_caps[i]}")
            if streak != 0:
                failures.append(f"step {i+1}: streak={streak} expected=0 after 429")

    finally:
        h.cleanup_interface(args.base_url, headers, INTERFACE_NAME)
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)

    if failures:
        print()
        print(f"FAIL: {len(failures)} AIMD violations:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print()
    print("PASS: AIMD cap halves on each 429 and floors at 1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
