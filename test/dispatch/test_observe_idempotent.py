#!/usr/bin/env python3
"""
§14 Tier B hermetic test — RateLimitController::Observe() idempotent merge.

Asserts the contract documented in `code/backend/engine/curlWrapper/rateLimitController.h`:

    > MUST be idempotent by replacement: a known field overwrites the prior
    > value, an unknown field preserves it.  Multiple Observe() calls per
    > request produce the same state as a single combined call.

The contract protects the future SSE refactor where headers and body arrive
separately — splitting one observation into two pieces (or three, or N) must
not change the merged state, and order must not matter when each call's set
of known fields is disjoint.

Drives the merge through POST /api/debug/test-observe-idempotent (debug
builds only) which spins up an ephemeral RateLimitController and applies the
observations in the order given.  Returns the resulting last_observation +
cap + streak so this test can compare scenarios.

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


# Sentinel for "unknown" fields — Observe() must preserve prior values when
# fed -1.  The C++ constructor of RateLimitObservation defaults every field
# to -1 / nullopt; the test endpoint forwards only fields the body sets.
UNKNOWN_INT = -1


def headers_only_obs(was_429: bool = False) -> dict:
    """Observation as it would arrive from a header-only parse — remaining_*
    + reset_in_sec populated, consumed_* unknown."""
    return {
        "remaining_requests": 1000,
        "remaining_input_tokens": 50000,
        "remaining_output_tokens": 30000,
        "requests_reset_in_sec": 60,
        "tokens_reset_in_sec": 60,
        "consumed_input_tokens": UNKNOWN_INT,
        "consumed_output_tokens": UNKNOWN_INT,
        "retry_after_ms": UNKNOWN_INT,
        "was_429": was_429,
    }


def body_only_obs(was_429: bool = False) -> dict:
    """Observation as it would arrive from a body-only parse — usage tokens
    populated, remaining_* / reset unknown."""
    return {
        "remaining_requests": UNKNOWN_INT,
        "remaining_input_tokens": UNKNOWN_INT,
        "remaining_output_tokens": UNKNOWN_INT,
        "requests_reset_in_sec": UNKNOWN_INT,
        "tokens_reset_in_sec": UNKNOWN_INT,
        "consumed_input_tokens": 200,
        "consumed_output_tokens": 100,
        "retry_after_ms": UNKNOWN_INT,
        "was_429": was_429,
    }


def combined_obs(was_429: bool = False) -> dict:
    """Single observation with both header and body fields populated."""
    return {
        "remaining_requests": 1000,
        "remaining_input_tokens": 50000,
        "remaining_output_tokens": 30000,
        "requests_reset_in_sec": 60,
        "tokens_reset_in_sec": 60,
        "consumed_input_tokens": 200,
        "consumed_output_tokens": 100,
        "retry_after_ms": UNKNOWN_INT,
        "was_429": was_429,
    }


def run_scenario(base_url: str, headers: dict, observations: list[dict],
                 initial_probe: int = 4, hard_cap: int = 48) -> dict:
    payload = {
        "initial_concurrency_probe": initial_probe,
        "hard_cap": hard_cap,
        "observations": observations,
    }
    r = requests.post(
        f"{base_url}/api/debug/test-observe-idempotent",
        json=payload, headers=headers, verify=False, timeout=10,
    )
    if r.status_code != 200:
        raise RuntimeError(f"endpoint returned {r.status_code}: {r.text[:200]}")
    body = r.json()
    if not body.get("ok"):
        raise RuntimeError(f"response missing ok=true: {body}")
    return body


def compare_last_observation(label: str, expected: dict, actual: dict, *, allow_reset_drift_sec: int = 2) -> bool:
    """Compare two last_observation dicts.  Reset times are wall-clock
    derived (now + reset_in_sec at observation time), so compare them with
    a small tolerance — different scenarios may invoke Observe at slightly
    different absolute times."""
    keys_exact = [
        "is_empty",
        "remaining_requests", "remaining_input_tokens",
        "remaining_output_tokens", "remaining_combined_tokens",
        "retry_after_ms", "consumed_input_tokens", "consumed_output_tokens",
    ]
    ok = True
    for key in keys_exact:
        if expected.get(key) != actual.get(key):
            print(f"FAIL [{label}]: {key} expected={expected.get(key)} actual={actual.get(key)}")
            ok = False
    keys_drift = ["requests_reset_in_sec", "tokens_reset_in_sec"]
    for key in keys_drift:
        ev = expected.get(key, -1)
        av = actual.get(key, -1)
        if ev == -1 and av == -1:
            continue
        if ev == -1 or av == -1:
            print(f"FAIL [{label}]: {key} expected={ev} actual={av} (one is unknown)")
            ok = False
            continue
        if abs(ev - av) > allow_reset_drift_sec:
            print(f"FAIL [{label}]: {key} expected≈{ev} actual={av} (drift > {allow_reset_drift_sec}s)")
            ok = False
    return ok


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}

    # --- Scenario A: single combined observation ---------------------------
    a = run_scenario(args.base_url, headers, [combined_obs(was_429=False)])
    a_last = a["last_observation"]
    a_cap = a["current_concurrency_cap"]
    a_streak = a["streak_since_last_429"]
    print(f"A (combined ×1): cap={a_cap} streak={a_streak} last={{rr={a_last['remaining_requests']}, "
          f"ri={a_last['remaining_input_tokens']}, ci={a_last['consumed_input_tokens']}}}")

    # --- Scenario B: split (headers, then body) ----------------------------
    b = run_scenario(args.base_url, headers, [headers_only_obs(False), body_only_obs(False)])
    b_last = b["last_observation"]
    b_cap = b["current_concurrency_cap"]
    b_streak = b["streak_since_last_429"]
    print(f"B (headers→body): cap={b_cap} streak={b_streak} last={{rr={b_last['remaining_requests']}, "
          f"ri={b_last['remaining_input_tokens']}, ci={b_last['consumed_input_tokens']}}}")

    # --- Scenario C: split reversed (body, then headers) -------------------
    c = run_scenario(args.base_url, headers, [body_only_obs(False), headers_only_obs(False)])
    c_last = c["last_observation"]
    c_cap = c["current_concurrency_cap"]
    c_streak = c["streak_since_last_429"]
    print(f"C (body→headers): cap={c_cap} streak={c_streak} last={{rr={c_last['remaining_requests']}, "
          f"ri={c_last['remaining_input_tokens']}, ci={c_last['consumed_input_tokens']}}}")

    # --- Assertions ---------------------------------------------------------
    # Idempotent merge contract: last_observation fields are identical across
    # all three scenarios (modulo small reset-time drift from wall-clock).
    ok = True
    if not compare_last_observation("A vs B", a_last, b_last):
        ok = False
    if not compare_last_observation("A vs C", a_last, c_last):
        ok = False
    if not compare_last_observation("B vs C", b_last, c_last):
        ok = False

    # Order-independence within the split case: B and C must be identical
    # in cap + streak too (same number of Observe calls, same was_429 sequence).
    if b_cap != c_cap:
        print(f"FAIL: B cap={b_cap} != C cap={c_cap} — split-order changed AIMD cap")
        ok = False
    if b_streak != c_streak:
        print(f"FAIL: B streak={b_streak} != C streak={c_streak} — split-order changed streak")
        ok = False

    # Documented expected difference: A advances streak once, B/C twice.
    # That's by design (Observe advances AIMD state per-call); the contract
    # is about field merging, not AIMD count.  Surface as info, not a check.
    print(f"INFO: A streak={a_streak}, B/C streak={b_streak} — expected: B/C made 2 Observe calls vs A's 1")

    # 429 ordering: a 429 anywhere in the sequence must halve cap and reset
    # streak, then subsequent non-429 advances the streak from 0 again.
    d = run_scenario(args.base_url, headers, [headers_only_obs(False), body_only_obs(was_429=True)])
    if d["streak_since_last_429"] != 0:
        print(f"FAIL: 429 in second position should reset streak to 0, got {d['streak_since_last_429']}")
        ok = False
    # initial_probe = 4 → after one 429: cap = max(1, 4 / 2) = 2
    if d["current_concurrency_cap"] != 2:
        print(f"FAIL: 429 should halve cap from 4 to 2, got {d['current_concurrency_cap']}")
        ok = False
    print(f"D (headers→body[429]): cap={d['current_concurrency_cap']} streak={d['streak_since_last_429']}")

    if ok:
        print("PASS: Observe() idempotent merge holds across combined / split / reverse-split scenarios")
        return 0
    else:
        print("FAIL: idempotence contract violated — see lines above")
        return 1


if __name__ == "__main__":
    sys.exit(main())
