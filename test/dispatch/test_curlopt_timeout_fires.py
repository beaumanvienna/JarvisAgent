#!/usr/bin/env python3
"""
§14 Tier B hermetic test — CURLOPT_TIMEOUT_MS actually aborts in-flight requests.

Asserts the integration of the size-aware budget with libcurl: when the
mock endpoint delays the response past the computed timeout, curl returns
CURLE_OPERATION_TIMEDOUT and AiRequestPool routes the failure into the
workflow task's Failed state.

Sets a tight rate_limit (min_seconds=1.0, max_seconds=5.0, all multipliers=0,
fixed_overhead=0.5) so timeoutMs = clamp(0.5 * safety, 1.0, 5.0) * 1000 → 1000ms.
Mock delays for 5000ms.  Curl must abort at ~1000ms and the workflow run
must transition to failed.

Runs against a Studio Debug build on the default port (8443).  Requires an
admin MCP key via --token or J9T_TOKEN.  Cleans up on exit (best-effort).
"""

import argparse
import os
import sys
import time
import urllib3

try:
    import requests
except ImportError:
    print("ERROR: requests package missing (pip install requests)")
    sys.exit(1)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _tier_b_helpers as h  # noqa: E402

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


PROVIDER_NAME  = "tier_b_timeout_provider"
INTERFACE_NAME = "tier_b_timeout_iface"

# Tight budget so curl aborts before the mock's 5s delay.
# Formula: ((est_in/1000)*0 + (4096/1000)*0 + 0.5) * 1 * 2.0 = 1.0s, clamped to [1, 5] = 1.0s.
# max_concurrency=1 keeps the formula's queue-depth multiplier from inflating the
# budget — this test deliberately exercises the tight-timeout corner.
RATE_LIMIT = {
    "initial_concurrency_probe": 1,
    "max_concurrency": 1,
    "max_retries_429": 0,
    "max_retries_transient": 0,
    "base_retry_ms": 100,
    "request_budget": {
        "per_1k_input_token_seconds": 0.0,
        "per_1k_output_token_seconds": 0.0,
        "fixed_overhead_seconds": 0.5,
        "safety_margin_factor": 2.0,
        "min_seconds": 1.0,
        "max_seconds": 5.0,
    },
}

EXPECTED_TIMEOUT_MS = 1000  # 0.5 * 1 (cap) * 2.0 = 1.0s; min/max clamp accepts as-is.
MOCK_DELAY_MS       = 5000  # 5x longer than the budget — guaranteed to trip.


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

    if not h.provision_provider(args.base_url, headers, PROVIDER_NAME, api_type="API1"):
        return 1

    mock_url = h.mock_endpoint_url(args.base_url, status=200,
                                    delay_ms=MOCK_DELAY_MS,
                                    header_fixture="openai_quota",
                                    body_fixture="openai_success")
    if not h.provision_interface(args.base_url, headers,
                                  name=INTERFACE_NAME, api_type="API1",
                                  model="gpt-4o-mini", mock_url=mock_url,
                                  key_name=PROVIDER_NAME, rate_limit=RATE_LIMIT):
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)
        return 1

    failures = []
    try:
        # Submit and time the round-trip.  Curl should abort at ~1s; the
        # workflow run transitions to failed shortly after.
        jcwf = h.build_adhoc_jcwf(workflow_id="tier_b_timeout",
                                   interface_name=INTERFACE_NAME)
        t0 = time.time()
        run_id = h.submit_adhoc(args.base_url, headers, jcwf)
        if not run_id:
            failures.append("run-adhoc failed")
            return 1

        state = h.poll_run_state(args.base_url, headers, run_id, timeout_s=10)
        elapsed = time.time() - t0
        print(f"  run={run_id} state={state} elapsed={elapsed:.2f}s")

        if state != "failed":
            failures.append(f"expected state=failed, got {state}")

        # Sanity check: elapsed should be much closer to EXPECTED_TIMEOUT_MS / 1000
        # than to MOCK_DELAY_MS / 1000.  Allow 3s slack for Crow worker scheduling
        # + workflow runtime processing.
        if elapsed >= MOCK_DELAY_MS / 1000.0:
            failures.append(
                f"elapsed={elapsed:.2f}s ≥ mock delay {MOCK_DELAY_MS/1000:.1f}s — "
                f"curl did NOT abort early (timeout did not fire)")

        # Verify the configured budget actually landed in QueryData.
        subs = h.get_recent_submissions(args.base_url, headers)
        if not subs:
            failures.append("no submissions captured")
        else:
            last = subs[0]
            print(f"  last submission: timeout_ms={last['timeout_ms']} "
                  f"quota_key={last['quota_key']!r}")
            if last["timeout_ms"] != EXPECTED_TIMEOUT_MS:
                failures.append(
                    f"timeout_ms={last['timeout_ms']} expected={EXPECTED_TIMEOUT_MS} "
                    f"— rate_limit override not applied?")

    finally:
        h.cleanup_interface(args.base_url, headers, INTERFACE_NAME)
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)

    if failures:
        print()
        print(f"FAIL: {len(failures)} timeout issues:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print()
    print("PASS: CURLOPT_TIMEOUT_MS aborts the in-flight request when mock delays past budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
