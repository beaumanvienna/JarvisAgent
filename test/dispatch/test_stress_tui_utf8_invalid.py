#!/usr/bin/env python3
"""
TUI ncurses stress test — malformed UTF-8 bytes, graceful catch required.

Drives one jarvisCpp JCWF (~140 ai_call tasks) with every reply replaced by
a hand-crafted byte salad: orphan continuation bytes (0x80-0xBF without
lead), truncated 2/3/4-byte sequences, illegal start bytes (0xFE / 0xFF),
overlong encodings (NUL as 0xC0 0x80, '/' as 0xC0 0xAF), surrogate-half
codepoints (U+D800/U+DC00 in UTF-8), and mid-character truncations
mirroring the 2026-04-27 substr-at-1024 bug.

The contract: j9t must SURVIVE.  The TUI's ncurses renderer must sanitize
or replace bad bytes — never feed raw garbage to ncurses (which can crash
or scramble the terminal).  Equally important: log/log.txt must remain
well-formed UTF-8 — downstream tools (dashboard, log viewers, dashboard
WebSocket consumers) all assume valid UTF-8 from the log emitter.

Verifies:
  - j9t process alive after ~140 tasks of bad-byte replies (HARD requirement).
  - Run reaches terminal state — failed is acceptable here (bad reply may
    cause the task to fail), but cancelled / hang is not.
  - log/log.txt bytes appended during the run decode cleanly as UTF-8
    (HARD requirement — sanitization layer must catch bad bytes BEFORE the
    log emitter).

Runs against a Studio Debug build on the default port (8443).  Requires an
admin MCP key via --token or J9T_TOKEN.
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
import _stress_tui_helpers as h  # noqa: E402

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

INTERFACE_NAME = "stress_tui_invalid"
FIXTURE_PATH = h.REPO_ROOT / "test" / "dispatch" / "fixtures" / "utf8_invalid.txt"
WORKFLOW_NAME = "jarvisCppDocu"  # 140 tasks; one workflow is enough — what
                                  # matters here is bad-byte coverage, not volume

RUN_TIMEOUT_S = 600.0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    args = parser.parse_args()
    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1
    if not FIXTURE_PATH.exists():
        print(f"FAIL: fixture missing at {FIXTURE_PATH}")
        return 1

    # Sanity-check the fixture really is invalid UTF-8 — otherwise we'd be
    # accidentally testing the heavy-but-valid path.
    with open(FIXTURE_PATH, "rb") as f:
        raw = f.read()
    try:
        raw.decode("utf-8")
        print(f"FAIL: fixture at {FIXTURE_PATH} decodes as valid UTF-8 — would not exercise sanitization")
        return 1
    except UnicodeDecodeError:
        pass  # good — fixture is malformed as intended

    headers = {"Authorization": f"Bearer {args.token}"}

    if not h.provision_test_interface(args.base_url, headers,
                                        name=INTERFACE_NAME,
                                        fixture_path=FIXTURE_PATH):
        return 1

    log_offset_at_start = h.log_file_size()
    failures = []
    run_id = None

    try:
        suffix = time.strftime("%H%M%S")
        jcwf = h.load_jarvisCpp_workflow(
            WORKFLOW_NAME,
            interface_override=INTERFACE_NAME,
            id_suffix=f"stress_invalid_{suffix}",
        )
        task_count = sum(1 for t in jcwf.get("tasks", {}).values()
                          if isinstance(t, dict) and t.get("type") == "ai_call")
        print(f"  loaded {WORKFLOW_NAME}: {task_count} ai_call tasks (id={jcwf.get('id')})")
        print(f"  fixture: {FIXTURE_PATH.name} ({len(raw)} bytes, malformed UTF-8 as intended)")

        run_id = h.submit_adhoc(args.base_url, headers, jcwf, "on_completion")
        if not run_id:
            failures.append("submit returned no run id")
            return _summarize(failures)
        print(f"  submitted run={run_id}")

        t0 = time.time()
        state = h.poll_run_state(args.base_url, headers, run_id, RUN_TIMEOUT_S)
        elapsed = time.time() - t0
        print(f"  terminal state: {state} (elapsed {elapsed:.1f}s)")

        if state is None:
            failures.append(f"run {run_id} did not reach terminal state within {RUN_TIMEOUT_S}s — possible hang")
        elif state == "cancelled":
            failures.append(f"run {run_id} terminated as cancelled — unexpected for this fixture")
        # 'succeeded' or 'failed' are both acceptable — bad bytes may cause
        # downstream parsing to reject (failed) but j9t survived.

        # HARD: j9t must still be alive.
        if not h.j9t_alive(args.base_url, headers):
            failures.append("j9t process is not alive after the run — TUI / renderer crash on bad bytes")
        else:
            print(f"  j9t alive (HTTP responding)")

        # HARD: log file must remain valid UTF-8 throughout.  Sanitization
        # is the contract.  If this fails, the dashboard's WebSocket log
        # stream and any UTF-8-aware reader downstream will choke.
        ok, err = h.log_tail_is_valid_utf8(start_offset=log_offset_at_start)
        new_bytes = h.log_file_size() - log_offset_at_start
        if not ok:
            failures.append(f"log/log.txt has invalid UTF-8 since the run started "
                             f"({new_bytes} new bytes appended): {err}")
        else:
            print(f"  log/log.txt: {new_bytes} new bytes appended, all valid UTF-8")

    finally:
        h.cleanup_test_interface(args.base_url, headers, INTERFACE_NAME)

    return _summarize(failures)


def _summarize(failures: list[str]) -> int:
    if failures:
        print()
        print(f"FAIL: {len(failures)} stress-test issues:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print()
    print("PASS: malformed UTF-8 stress completed — j9t alive, log stays valid UTF-8 (sanitization works)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
