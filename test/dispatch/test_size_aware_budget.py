#!/usr/bin/env python3
"""
§14 Tier B hermetic test — size-aware in-flight budget formula.

Asserts that QueryData::m_TimeoutMs computed by AiRequestPool::Submit matches
the formula in `AiRequestPool::Submit`:

    seconds = (in_tokens/1000) * per_1k_input
            + (out_tokens/1000) * per_1k_output
            + fixed_overhead_seconds
    seconds *= max_concurrency       # worst-case queue-depth multiplier
    seconds *= safety_margin_factor  # token-rate variance headroom
    seconds  = clamp(seconds, min_seconds, max_seconds)
    timeoutMs = long(seconds * 1000)

Drives several different prob sizes through a real adhoc workflow against
the mock-AI-response endpoint, then reads /api/debug/recent-submissions and
verifies each submission's `timeout_ms` matches the formula given the
returned `estimated_input_tokens` and the configured rate_limit block.

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


PROVIDER_NAME = "tier_b_size_provider"
INTERFACE_NAME = "tier_b_size_iface"

# Tight, well-defined budget so we can assert exact numbers.  Default
# m_DefaultOutputTokens (4096) is used for max_output_tokens since we don't
# override settings.max_tokens in the JCWF.  `max_concurrency` feeds the
# budget formula (worst-case queue-depth multiplier on serializing backends)
# — kept low so the medium/large-input cases don't all clamp to max_seconds.
RATE_LIMIT = {
    "initial_concurrency_probe": 4,
    "max_concurrency": 2,
    "max_retries_429": 2,
    "max_retries_transient": 1,
    "base_retry_ms": 100,
    "request_budget": {
        "per_1k_input_token_seconds": 0.20,
        "per_1k_output_token_seconds": 0.80,
        "fixed_overhead_seconds": 5.0,
        "safety_margin_factor": 3.0,
        "min_seconds": 30.0,
        "max_seconds": 600.0,
    },
}
MAX_CONCURRENCY = RATE_LIMIT["max_concurrency"]

DEFAULT_OUTPUT_TOKENS = 4096  # ConfigParser::ApiInterface::m_DefaultOutputTokens default


def expected_timeout_ms(estimated_input_tokens: int, *,
                        per_1k_in: float, per_1k_out: float,
                        fixed: float, safety: float,
                        min_s: float, max_s: float,
                        max_concurrency: int,
                        out_tokens: int = DEFAULT_OUTPUT_TOKENS) -> int:
    seconds = (estimated_input_tokens / 1000.0) * per_1k_in \
            + (out_tokens / 1000.0) * per_1k_out \
            + fixed
    seconds *= max(1, max_concurrency)  # queue-depth multiplier
    seconds *= safety
    seconds = max(min_s, min(max_s, seconds))
    return int(seconds * 1000.0)


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
        # Three distinct sizes drive the formula across a reasonable range.
        # Tiny → likely clamped to min_seconds.
        # Medium → no clamp, exercises the linear region.
        # Large → grows with input but typically still no clamp.
        sizes = [10, 4_000, 40_000]

        for size in sizes:
            prob = "x" * size
            jcwf = h.build_adhoc_jcwf(
                workflow_id=f"tier_b_size_{size}",
                interface_name=INTERFACE_NAME,
                prob_text=prob,
            )
            run_id = h.submit_adhoc(args.base_url, headers, jcwf)
            if not run_id:
                failures.append(f"size={size}: run-adhoc failed")
                continue
            state = h.poll_run_state(args.base_url, headers, run_id, timeout_s=20)
            print(f"  size={size:6d} run={run_id} state={state}")

        # Pull the dispatcher's recent submissions and validate the formula
        # for the most recent N entries (newest-first ordering).
        rl = RATE_LIMIT["request_budget"]
        subs = h.get_recent_submissions(args.base_url, headers)
        if not subs:
            failures.append("no submissions captured — dispatcher never saw the requests")

        # Map by URL so we can identify which submission corresponds to which
        # workflow.  All three workflows hit the same URL though, so we just
        # take the most recent len(sizes) submissions in chronological order
        # (oldest of the recent batch first).
        recent = list(reversed(subs[:len(sizes)]))  # oldest-first within the batch

        if len(recent) != len(sizes):
            failures.append(
                f"expected {len(sizes)} recent submissions, found {len(recent)}: "
                f"{[s['quota_key'] for s in subs[:5]]}"
            )

        print()
        print(f"  rate_limit: per_1k_in={rl['per_1k_input_token_seconds']} "
              f"per_1k_out={rl['per_1k_output_token_seconds']} "
              f"fixed={rl['fixed_overhead_seconds']} "
              f"safety={rl['safety_margin_factor']} "
              f"min={rl['min_seconds']}s max={rl['max_seconds']}s")
        print()

        for i, (size, sub) in enumerate(zip(sizes, recent)):
            est = sub["estimated_input_tokens"]
            actual_ms = sub["timeout_ms"]
            expected_ms = expected_timeout_ms(
                est,
                per_1k_in=rl["per_1k_input_token_seconds"],
                per_1k_out=rl["per_1k_output_token_seconds"],
                fixed=rl["fixed_overhead_seconds"],
                safety=rl["safety_margin_factor"],
                min_s=rl["min_seconds"],
                max_s=rl["max_seconds"],
                max_concurrency=MAX_CONCURRENCY,
            )
            ok = actual_ms == expected_ms
            tag = "ok" if ok else "FAIL"
            print(f"  [{tag}] size={size:6d} estimated_in={est:7d} "
                  f"actual_ms={actual_ms:8d} expected_ms={expected_ms:8d}")
            if not ok:
                failures.append(
                    f"size={size}: timeout_ms mismatch — "
                    f"actual={actual_ms} expected={expected_ms}"
                )

    finally:
        h.cleanup_interface(args.base_url, headers, INTERFACE_NAME)
        h.cleanup_provider(args.base_url, headers, PROVIDER_NAME)

    if failures:
        print()
        print(f"FAIL: {len(failures)} formula mismatches:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print()
    print("PASS: size-aware budget formula matches §6.2 across all submitted sizes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
