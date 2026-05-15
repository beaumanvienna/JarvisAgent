#!/usr/bin/env python3
"""
§14 Tier B hermetic test — IRateLimitStrategy::Get() dispatch table.

Asserts that each InterfaceType maps to the correct strategy in the switch
at `engine/curlWrapper/rateLimitStrategy.cpp:377`.  Drives every InterfaceType
through POST /api/debug/parse-rate-limit-headers (already shipped for Tier A)
with two distinct header shapes — OpenAI-style and Anthropic-style — and
verifies each one is parsed only by the strategy that owns its shape.

Existing test_rate_limit_observation_parse.py covers the per-strategy parse
details; this test focuses on the *dispatch* — does Get(API4) actually return
the Anthropic strategy, etc.

Dispatch table (per rateLimitStrategy.cpp):
    API1, API2, API6 → OpenAI strategy   (InitialConcurrencyProbe = 8)
    API3, API5, Test → Empty strategy    (InitialConcurrencyProbe = 4)
    API4             → Anthropic strategy (InitialConcurrencyProbe = 4)

Zero network calls outside localhost.  Runs against a Studio Debug build on
the default port (8443).  Requires an admin MCP key via --token or J9T_TOKEN.
"""

import argparse
import datetime
import os
import sys
import urllib3

try:
    import requests
except ImportError:
    print("ERROR: requests package missing (pip install requests)")
    sys.exit(1)

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


# Minimal OpenAI-shaped header buffer — the OpenAI strategy reads
# x-ratelimit-{remaining,reset}-{requests,tokens}.
def openai_headers() -> str:
    return (
        "HTTP/2 200\r\n"
        "x-ratelimit-remaining-requests: 1000\r\n"
        "x-ratelimit-remaining-tokens: 50000\r\n"
        "x-ratelimit-reset-requests: 60s\r\n"
        "x-ratelimit-reset-tokens: 60s\r\n"
        "\r\n"
    )


# Minimal Anthropic-shaped header buffer — the Anthropic strategy reads
# anthropic-ratelimit-{requests,tokens,input-tokens,output-tokens}-{remaining,reset}.
# Reset times are wall-clock ISO 8601, capped at 7200s in the future.
def anthropic_headers() -> str:
    reset_at = (datetime.datetime.now(datetime.timezone.utc)
                + datetime.timedelta(seconds=60)).strftime("%Y-%m-%dT%H:%M:%SZ")
    return (
        "HTTP/2 200\r\n"
        f"anthropic-ratelimit-requests-remaining: 50\r\n"
        f"anthropic-ratelimit-requests-reset: {reset_at}\r\n"
        f"anthropic-ratelimit-input-tokens-remaining: 30000\r\n"
        f"anthropic-ratelimit-input-tokens-reset: {reset_at}\r\n"
        f"anthropic-ratelimit-output-tokens-remaining: 10000\r\n"
        f"anthropic-ratelimit-output-tokens-reset: {reset_at}\r\n"
        "\r\n"
    )


def parse(base_url: str, headers: dict, *, interface_type: str,
          header_buffer: str, model: str = "") -> dict:
    payload = {
        "interface_type": interface_type,
        "model": model,
        "header_buffer": header_buffer,
        "body": "",
        "http_status": 200,
    }
    r = requests.post(
        f"{base_url}/api/debug/parse-rate-limit-headers",
        json=payload, headers=headers, verify=False, timeout=10,
    )
    if r.status_code != 200:
        raise RuntimeError(f"endpoint returned {r.status_code}: {r.text[:200]}")
    return r.json()


# Expected strategy per InterfaceType (matches the switch in rateLimitStrategy.cpp).
EXPECTED_STRATEGY = {
    "API1": "openai",
    "API2": "openai",
    "API3": "empty",
    "API4": "anthropic",
    "API5": "empty",
    "API6": "openai",
}

EXPECTED_PROBE = {
    "openai": 8,
    "anthropic": 4,
    "empty": 4,
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
    failures = []

    # --- 1. InitialConcurrencyProbe per InterfaceType ----------------------
    # The probe is a per-strategy constant; verifying it discriminates OpenAI
    # (=8) from Empty/Anthropic (=4).  Combined with the header-parse asserts
    # below, this pins down the dispatch table fully.
    print("--- InitialConcurrencyProbe per InterfaceType ---")
    for itype, strategy in EXPECTED_STRATEGY.items():
        result = parse(args.base_url, headers,
                       interface_type=itype, header_buffer="", model="")
        actual_probe = result.get("initial_concurrency_probe")
        expected_probe = EXPECTED_PROBE[strategy]
        if actual_probe != expected_probe:
            failures.append(
                f"{itype}: expected probe={expected_probe} (strategy={strategy}), "
                f"actual={actual_probe}"
            )
            print(f"  {itype} → probe={actual_probe} (expected {expected_probe})  FAIL")
        else:
            print(f"  {itype} → probe={actual_probe}  ok ({strategy})")

    # --- 2. OpenAI-shaped headers ------------------------------------------
    # Only API1/2/6 (OpenAI strategy) should parse non-empty obs.
    print("\n--- OpenAI-shaped headers ---")
    oai_buf = openai_headers()
    for itype, strategy in EXPECTED_STRATEGY.items():
        result = parse(args.base_url, headers,
                       interface_type=itype, header_buffer=oai_buf, model="gpt-4o-mini")
        obs = result["observation"]
        is_empty = obs["is_empty"]
        if strategy == "openai":
            # Should parse remaining_requests + remaining_combined_tokens
            if is_empty:
                failures.append(f"{itype} (openai strategy): OpenAI headers parsed as empty obs")
                print(f"  {itype} → empty=True  FAIL (openai strategy should parse OpenAI headers)")
            elif obs["remaining_requests"] != 1000:
                failures.append(f"{itype}: remaining_requests={obs['remaining_requests']} (expected 1000)")
                print(f"  {itype} → rr={obs['remaining_requests']}  FAIL")
            else:
                print(f"  {itype} → rr={obs['remaining_requests']} rt={obs['remaining_combined_tokens']}  ok")
        else:
            # Empty / Anthropic strategy: should NOT recognize OpenAI headers.
            if not is_empty:
                failures.append(f"{itype} ({strategy} strategy): unexpectedly parsed OpenAI headers")
                print(f"  {itype} → empty=False rr={obs['remaining_requests']}  FAIL")
            else:
                print(f"  {itype} → empty=True  ok ({strategy} ignores OpenAI shape)")

    # --- 3. Anthropic-shaped headers ---------------------------------------
    # Only API4 (Anthropic strategy) should parse non-empty obs.
    print("\n--- Anthropic-shaped headers ---")
    ant_buf = anthropic_headers()
    for itype, strategy in EXPECTED_STRATEGY.items():
        result = parse(args.base_url, headers,
                       interface_type=itype, header_buffer=ant_buf, model="claude-sonnet-4-6")
        obs = result["observation"]
        is_empty = obs["is_empty"]
        if strategy == "anthropic":
            if is_empty:
                failures.append(f"{itype} (anthropic strategy): Anthropic headers parsed as empty obs")
                print(f"  {itype} → empty=True  FAIL (anthropic strategy should parse Anthropic headers)")
            elif obs["remaining_requests"] != 50:
                failures.append(f"{itype}: remaining_requests={obs['remaining_requests']} (expected 50)")
                print(f"  {itype} → rr={obs['remaining_requests']}  FAIL")
            else:
                print(f"  {itype} → rr={obs['remaining_requests']} ri={obs['remaining_input_tokens']} "
                      f"ro={obs['remaining_output_tokens']}  ok")
        else:
            if not is_empty:
                failures.append(f"{itype} ({strategy} strategy): unexpectedly parsed Anthropic headers")
                print(f"  {itype} → empty=False rr={obs['remaining_requests']}  FAIL")
            else:
                print(f"  {itype} → empty=True  ok ({strategy} ignores Anthropic shape)")

    print()
    if failures:
        print(f"FAIL: {len(failures)} dispatch errors:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("PASS: every InterfaceType dispatches to the expected strategy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
