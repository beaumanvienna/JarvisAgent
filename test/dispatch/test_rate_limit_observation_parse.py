#!/usr/bin/env python3
"""
Hermetic strategy-parser contract test (§14 Tier A foundation).

Drives every per-provider IRateLimitStrategy through canned header fixtures
via POST /api/debug/parse-rate-limit-headers (debug builds only). Asserts:

  - OpenAI duration parsing (`200ms` / `6s` / `1m30s`) yields the right
    seconds-from-now on the requests/tokens reset fields.
  - OpenAI 429 produces retry_after_ms = retry-after header * 1000,
    other fields stay -1 / unknown.
  - Anthropic split input/output token quotas surface separately.
  - Anthropic ISO 8601 reset times parse to a future seconds-from-now.
  - Anthropic 429 retry-after (seconds) maps to retry_after_ms.
  - Empty strategy (Gemini API3, Bedrock API5, Test) returns is_empty=true
    on any header buffer.
  - DeriveQuotaKey(model) produces the expected family per provider.
  - InitialConcurrencyProbe() returns the documented per-provider default.

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

FIXTURES_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "fixtures", "headers"))


def load_fixture(name: str, substitutions: dict | None = None) -> str:
    with open(os.path.join(FIXTURES_DIR, name), "r", encoding="utf-8") as f:
        text = f.read()
    # Substitute {{KEY}} placeholders. Used for Anthropic ISO 8601 reset times,
    # which the parser caps at 7200s from now — a static fixture timestamp
    # would either be in the past (rejected) or beyond the cap (rejected too).
    if substitutions:
        for key, value in substitutions.items():
            text = text.replace("{{" + key + "}}", value)
    # Convert LF to CRLF — production header buffers are CRLF, our fixtures
    # are stored as LF for ergonomic editing.  The parser handles both, but
    # serializing as CRLF in the request mirrors what curl would produce.
    return text.replace("\n", "\r\n")


def iso8601_seconds_from_now(seconds: int) -> str:
    """Format a UTC ISO 8601 timestamp `seconds` in the future, in the shape
    Anthropic ships (`YYYY-MM-DDThh:mm:ssZ`)."""
    target = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(seconds=seconds)
    # Strip microseconds — the parser uses %d-%d-%dT%d:%d:%dZ, no fraction.
    return target.strftime("%Y-%m-%dT%H:%M:%SZ")


def parse(base_url: str, headers: dict, *, interface_type: str, fixture: str = None,
          fixture_subs: dict | None = None,
          model: str = "", body: str = "", http_status: int = 200) -> dict:
    payload = {
        "interface_type": interface_type,
        "model": model,
        "header_buffer": load_fixture(fixture, fixture_subs) if fixture else "",
        "body": body,
        "http_status": http_status,
    }
    r = requests.post(
        f"{base_url}/api/debug/parse-rate-limit-headers",
        json=payload, headers=headers, verify=False, timeout=10,
    )
    if r.status_code != 200:
        raise RuntimeError(f"endpoint returned {r.status_code}: {r.text[:200]}")
    return r.json()


def expect(label: str, actual, expected) -> bool:
    if actual != expected:
        print(f"FAIL [{label}]: expected {expected!r}, got {actual!r}")
        return False
    return True


def expect_in_range(label: str, actual: int, lo: int, hi: int) -> bool:
    if not (lo <= actual <= hi):
        print(f"FAIL [{label}]: expected {actual} in [{lo}, {hi}]")
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}
    failures: list[str] = []

    # ------------------------------------------------------------------
    # OpenAI strategy (API1 chat, API2 responses, API6 Azure OpenAI all share)
    # ------------------------------------------------------------------
    for itype in ("API1", "API2", "API6"):
        r = parse(args.base_url, headers, interface_type=itype,
                  fixture="openai_quota.txt", model="gpt-4o-mini")
        obs = r["observation"]
        ok = True
        ok &= expect(f"{itype} OpenAI quota: remaining_requests",
                     obs["remaining_requests"], 4998)
        ok &= expect(f"{itype} OpenAI quota: remaining_combined_tokens",
                     obs["remaining_combined_tokens"], 798543)
        ok &= expect(f"{itype} OpenAI quota: retry_after_ms",
                     obs["retry_after_ms"], -1)
        # 1m30s = 90s — the parser converts to a steady_clock + offset, then
        # the endpoint computes seconds-from-now.  Allow ±2s slop for the
        # request round-trip so the test is stable on slow machines.
        ok &= expect_in_range(f"{itype} OpenAI quota: requests_reset_in_sec ≈ 90",
                              obs["requests_reset_in_sec"], 88, 90)
        ok &= expect_in_range(f"{itype} OpenAI quota: tokens_reset_in_sec ≈ 6",
                              obs["tokens_reset_in_sec"], 4, 6)
        ok &= expect(f"{itype} OpenAI quota: is_empty", obs["is_empty"], False)
        ok &= expect(f"{itype} OpenAI quota: quota_key (gpt-4o-mini → gpt-4o)",
                     r["quota_key"], "gpt-4o")
        ok &= expect(f"{itype} OpenAI quota: initial_concurrency_probe",
                     r["initial_concurrency_probe"], 8)
        if not ok:
            failures.append(f"{itype} OpenAI quota fixture")

    r = parse(args.base_url, headers, interface_type="API1",
              fixture="openai_429.txt", model="gpt-4.1", http_status=429)
    obs = r["observation"]
    ok = True
    ok &= expect("API1 OpenAI 429: retry_after_ms (12s → 12000ms)",
                 obs["retry_after_ms"], 12000)
    ok &= expect("API1 OpenAI 429: remaining_requests",
                 obs["remaining_requests"], -1)
    ok &= expect("API1 OpenAI 429: remaining_combined_tokens",
                 obs["remaining_combined_tokens"], -1)
    ok &= expect("API1 OpenAI 429: is_empty", obs["is_empty"], False)
    # gpt-4.1 has only one hyphen → DeriveQuotaKey returns the model as-is.
    ok &= expect("API1 OpenAI 429: quota_key (gpt-4.1 stays as-is)",
                 r["quota_key"], "gpt-4.1")
    if not ok:
        failures.append("API1 OpenAI 429 fixture")

    # ------------------------------------------------------------------
    # Anthropic strategy (API4)
    # ------------------------------------------------------------------
    # Anthropic ISO 8601 reset must land within the parser's 0..7200s window —
    # use 60 s from now for a stable, easy-to-assert value.
    reset_iso = iso8601_seconds_from_now(60)
    r = parse(args.base_url, headers, interface_type="API4",
              fixture="anthropic_quota.txt", fixture_subs={"RESET_AT_ISO": reset_iso},
              model="claude-sonnet-4-6")
    obs = r["observation"]
    ok = True
    ok &= expect("API4 Anthropic quota: remaining_requests",
                 obs["remaining_requests"], 998)
    ok &= expect("API4 Anthropic quota: remaining_input_tokens",
                 obs["remaining_input_tokens"], 89500)
    ok &= expect("API4 Anthropic quota: remaining_output_tokens",
                 obs["remaining_output_tokens"], 17900)
    ok &= expect("API4 Anthropic quota: retry_after_ms",
                 obs["retry_after_ms"], -1)
    # 60 s from now → seconds-from-now should land in [55, 60]; allow ±5s slop
    # for round-trip + clock-resolution.
    ok &= expect_in_range("API4 Anthropic quota: requests_reset_in_sec ≈ 60",
                          obs["requests_reset_in_sec"], 55, 60)
    ok &= expect_in_range("API4 Anthropic quota: tokens_reset_in_sec ≈ 60",
                          obs["tokens_reset_in_sec"], 55, 60)
    ok &= expect("API4 Anthropic quota: is_empty", obs["is_empty"], False)
    ok &= expect("API4 Anthropic quota: quota_key (claude-sonnet-4-6 → claude-sonnet)",
                 r["quota_key"], "claude-sonnet")
    ok &= expect("API4 Anthropic quota: quota_key opus",
                 parse(args.base_url, headers, interface_type="API4",
                       fixture="empty.txt", model="claude-opus-4-7")["quota_key"],
                 "claude-opus")
    ok &= expect("API4 Anthropic quota: initial_concurrency_probe",
                 r["initial_concurrency_probe"], 4)
    if not ok:
        failures.append("API4 Anthropic quota fixture")

    r = parse(args.base_url, headers, interface_type="API4",
              fixture="anthropic_429.txt", model="claude-sonnet-4-6", http_status=429)
    obs = r["observation"]
    ok = True
    ok &= expect("API4 Anthropic 429: retry_after_ms (30s → 30000ms)",
                 obs["retry_after_ms"], 30000)
    ok &= expect("API4 Anthropic 429: remaining_requests",
                 obs["remaining_requests"], -1)
    ok &= expect("API4 Anthropic 429: is_empty", obs["is_empty"], False)
    if not ok:
        failures.append("API4 Anthropic 429 fixture")

    # ------------------------------------------------------------------
    # Empty strategy (API3 Gemini, API5 Bedrock, Test) — providers that ship
    # no proactive feedback. Any header buffer should yield an empty observation.
    # ------------------------------------------------------------------
    for itype in ("API3", "API5", "Test"):
        # Even a populated OpenAI-flavoured buffer should be ignored.
        r = parse(args.base_url, headers, interface_type=itype,
                  fixture="openai_quota.txt", model="some-model")
        obs = r["observation"]
        ok = True
        ok &= expect(f"{itype} Empty: is_empty (ignores headers)", obs["is_empty"], True)
        ok &= expect(f"{itype} Empty: remaining_requests",
                     obs["remaining_requests"], -1)
        ok &= expect(f"{itype} Empty: retry_after_ms",
                     obs["retry_after_ms"], -1)
        if not ok:
            failures.append(f"{itype} Empty (with OpenAI buffer)")

        # Empty buffer also yields empty observation.
        r = parse(args.base_url, headers, interface_type=itype,
                  fixture="empty.txt", model="")
        ok = expect(f"{itype} Empty (empty buffer): is_empty",
                    r["observation"]["is_empty"], True)
        if not ok:
            failures.append(f"{itype} Empty (empty buffer)")

    # ------------------------------------------------------------------
    # Bad input handling
    # ------------------------------------------------------------------
    r = requests.post(
        f"{args.base_url}/api/debug/parse-rate-limit-headers",
        json={"interface_type": "ZZZBOGUS"},
        headers=headers, verify=False, timeout=10,
    )
    if r.status_code != 400:
        failures.append(f"unknown interface_type should 400, got {r.status_code}")

    r = requests.post(
        f"{args.base_url}/api/debug/parse-rate-limit-headers",
        json={},
        headers=headers, verify=False, timeout=10,
    )
    if r.status_code != 400:
        failures.append(f"missing interface_type should 400, got {r.status_code}")

    # ------------------------------------------------------------------
    if failures:
        print(f"\nFAIL: {len(failures)} assertion group(s) failed:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print("OK: all rate-limit strategy parsers produce expected RateLimitObservation values "
          "(OpenAI duration parsing, 429 retry-after, Anthropic split token quotas + ISO 8601 resets, "
          "Empty strategy on Gemini / Bedrock / Test, DeriveQuotaKey + InitialConcurrencyProbe).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
