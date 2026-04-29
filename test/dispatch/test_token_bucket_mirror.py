#!/usr/bin/env python3
"""
§14 Tier B hermetic test — token-bucket mirror denies admission.

Asserts the token-bucket projection branch of `RateLimitController::ShouldAdmit`
(rateLimitController.cpp:80-103): when the controller's last observation
shows remaining_input_tokens / remaining_output_tokens / remaining_requests
at zero AND the matching reset time is still in the future, ShouldAdmit
denies the next admission until the reset elapses.

Drives one successful response with `remaining=0` via the mock endpoint, then
submits a follow-up ai_call within the reset window and verifies the
dispatcher's `dispatcher_total_throttled` counter incremented.

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


PROVIDER_NAME  = "tier_b_bucket_provider"
INTERFACE_NAME = "tier_b_bucket_iface"

RATE_LIMIT = {
    "initial_concurrency_probe": 4,
    "max_concurrency": 16,
    "max_retries_429": 0,
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

    # Step 1 mock URL: 200 with remaining_*=0 + reset 30s in the future.
    # Anthropic strategy parses the headers and feeds Observe(was_429=false).
    # The controller's last_observation now says "bucket empty until reset_at".
    mock_url_zero = h.mock_endpoint_url(args.base_url, status=200,
                                         reset_in_sec=30,
                                         header_fixture="anthropic_zero_quota",
                                         body_fixture="anthropic_success")
    if not h.provision_interface(args.base_url, headers,
                                  name=INTERFACE_NAME, api_type="API4",
                                  model="claude-sonnet-4-6", mock_url=mock_url_zero,
                                  key_name=PROVIDER_NAME, rate_limit=RATE_LIMIT):
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)
        return 1

    failures = []
    try:
        # First request: succeeds, observation populates the controller.
        jcwf1 = h.build_adhoc_jcwf(workflow_id="tier_b_bucket_first",
                                    interface_name=INTERFACE_NAME)
        run1 = h.submit_adhoc(args.base_url, headers, jcwf1)
        state1 = h.poll_run_state(args.base_url, headers, run1, timeout_s=15)
        print(f"  first run={run1} state={state1}")

        # Snapshot the controller — should now show remaining=0 + reset future.
        signals_after_first = h.get_signals(args.base_url, headers)
        ctrl_after_first = h.find_controller(signals_after_first, "sonnet") \
                        or h.find_controller(signals_after_first, "localhost")
        if ctrl_after_first is None:
            failures.append("controller not present after first request")
        else:
            print(f"  after-first: quota_key={ctrl_after_first['quota_key']!r} "
                  f"rr={ctrl_after_first.get('remaining_requests')} "
                  f"req_reset_in_sec={ctrl_after_first.get('req_reset_in_sec')}")
            if ctrl_after_first.get("remaining_requests") != 0:
                failures.append(
                    f"after-first: remaining_requests={ctrl_after_first.get('remaining_requests')} "
                    f"(expected 0 from fixture)")
            if ctrl_after_first.get("req_reset_in_sec", 0) <= 0:
                failures.append(
                    f"after-first: req_reset_in_sec={ctrl_after_first.get('req_reset_in_sec')} "
                    f"(expected > 0 — reset should be in the future)")

        # Capture throttle counter just before the second request so we
        # observe the increment caused by ShouldAdmit denying.
        before_throttled = signals_after_first.get("dispatcher_total_throttled", 0)

        # Second request: should be throttled — controller knows bucket is empty.
        jcwf2 = h.build_adhoc_jcwf(workflow_id="tier_b_bucket_second",
                                    interface_name=INTERFACE_NAME)
        run2 = h.submit_adhoc(args.base_url, headers, jcwf2)
        # Don't poll for terminal state — the second request will be parked
        # in inbox until reset elapses.  Just wait briefly for at least one
        # DrainInbox pass to land the throttle decision.
        time.sleep(2.0)

        signals_after_second = h.get_signals(args.base_url, headers)
        after_throttled = signals_after_second.get("dispatcher_total_throttled", 0)
        delta = after_throttled - before_throttled
        print(f"  throttled before={before_throttled} after={after_throttled} delta={delta}")

        if delta < 1:
            failures.append(
                f"dispatcher_total_throttled did not increment (before={before_throttled} "
                f"after={after_throttled}) — ShouldAdmit may not be denying")

        # Cancel the parked second request so cleanup doesn't have to wait
        # 30s for the reset.  Run might still be pending; cancel via REST.
        try:
            requests.post(f"{args.base_url}/api/workflow-runs/{run2}/cancel",
                          headers=headers, verify=False, timeout=5)
        except Exception:
            pass

    finally:
        h.cleanup_interface(args.base_url, headers, INTERFACE_NAME)
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)

    if failures:
        print()
        print(f"FAIL: {len(failures)} bucket-mirror issues:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print()
    print("PASS: token-bucket mirror denies admission while remaining=0 and reset is future")
    return 0


if __name__ == "__main__":
    sys.exit(main())
