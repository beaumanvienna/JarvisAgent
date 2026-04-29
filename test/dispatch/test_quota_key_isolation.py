#!/usr/bin/env python3
"""
§14 Tier B hermetic test — quota-key controller isolation.

Asserts that two AI interfaces hitting the SAME host but different model
families get INDEPENDENT controllers in CurlMultiDispatcher.  This is the
"Anthropic Sonnet vs Anthropic Opus on api.anthropic.com" scenario from
`AI call performance optimization.md` §3.3 — one host, two AIMD signals.

Drives one ai_call per interface against the localhost mock endpoint, then
reads /api/debug/signals dispatcher_controllers[] and verifies two distinct
quota_keys with independent state.

Runs against a Studio Debug build on the default port (8443).  Requires an
admin MCP key via --token or J9T_TOKEN.  Cleans up the providers + interfaces
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


PROVIDER_NAME = "tier_b_isolation_provider"
IFACE_SONNET  = "tier_b_iface_sonnet"
IFACE_OPUS    = "tier_b_iface_opus"


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

    mock_url = h.mock_endpoint_url(args.base_url, status=200,
                                    header_fixture="anthropic_quota",
                                    body_fixture="anthropic_success")
    if not h.provision_interface(args.base_url, headers,
                                  name=IFACE_SONNET, api_type="API4",
                                  model="claude-sonnet-4-6", mock_url=mock_url,
                                  key_name=PROVIDER_NAME):
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)
        return 1
    if not h.provision_interface(args.base_url, headers,
                                  name=IFACE_OPUS, api_type="API4",
                                  model="claude-opus-4-7", mock_url=mock_url,
                                  key_name=PROVIDER_NAME):
        h.cleanup_interface(args.base_url, headers, IFACE_SONNET)
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)
        return 1

    failures = []
    try:
        # One ai_call per interface — the dispatcher creates the controller
        # the first time DrainInbox sees a quota_key it doesn't know yet.
        for iface in (IFACE_SONNET, IFACE_OPUS):
            jcwf = h.build_adhoc_jcwf(workflow_id=f"tier_b_qk_{iface}",
                                       interface_name=iface)
            run_id = h.submit_adhoc(args.base_url, headers, jcwf)
            if not run_id:
                failures.append(f"{iface}: run-adhoc failed")
                continue
            state = h.poll_run_state(args.base_url, headers, run_id, timeout_s=15)
            print(f"  {iface} run={run_id} state={state}")

        signals = h.get_signals(args.base_url, headers)
        controllers = signals.get("dispatcher_controllers", [])

        # Find the two entries.  Quota key is "<host>|<modelFamily>"; both
        # entries share host=localhost (or localhost:8443) but differ in family.
        keys = [c["quota_key"] for c in controllers]
        print(f"  controllers seen: {keys}")

        # The Anthropic strategy's DeriveQuotaKey for "claude-sonnet-4-6" is
        # the family substring (typically "claude-sonnet" or similar) —
        # don't pin the exact format, just assert "sonnet" is in one and
        # "opus" is in the other.
        sonnet_match = [c for c in controllers if "sonnet" in c["quota_key"].lower()]
        opus_match   = [c for c in controllers if "opus"   in c["quota_key"].lower()]

        if len(sonnet_match) != 1:
            failures.append(f"expected exactly 1 sonnet controller, got {len(sonnet_match)}: {keys}")
        if len(opus_match) != 1:
            failures.append(f"expected exactly 1 opus controller, got {len(opus_match)}: {keys}")

        if sonnet_match and opus_match:
            sk = sonnet_match[0]["quota_key"]
            ok = opus_match[0]["quota_key"]
            if sk == ok:
                failures.append(f"sonnet and opus quota_keys are identical ({sk})")
            else:
                print(f"  sonnet quota_key = {sk!r}")
                print(f"  opus   quota_key = {ok!r}")
                # Independence: verify they have separate state — at minimum
                # different quota_key strings, ideally different last_observation.
                # A non-zero remaining (from anthropic_quota fixture's 998) on
                # both confirms each controller observed its own response.
                sonnet_rem = sonnet_match[0].get("remaining_requests", -1)
                opus_rem   = opus_match[0].get("remaining_requests", -1)
                print(f"  sonnet remaining_requests = {sonnet_rem}")
                print(f"  opus   remaining_requests = {opus_rem}")
                # anthropic_quota.txt has remaining=998; both controllers should
                # have observed it independently.
                if sonnet_rem != 998:
                    failures.append(f"sonnet remaining_requests={sonnet_rem} (expected 998 from fixture)")
                if opus_rem != 998:
                    failures.append(f"opus remaining_requests={opus_rem} (expected 998 from fixture)")

    finally:
        h.cleanup_interface(args.base_url, headers, IFACE_OPUS)
        h.cleanup_interface(args.base_url, headers, IFACE_SONNET)
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)

    if failures:
        print()
        print(f"FAIL: {len(failures)} isolation issues:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print()
    print("PASS: same host + different model families produce independent controllers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
