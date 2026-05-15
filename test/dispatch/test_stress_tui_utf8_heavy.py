#!/usr/bin/env python3
"""
TUI ncurses stress test — heavy valid UTF-8 across 3-way concurrent jarvisCpp.

Drives all three jarvisCppCyberSecAudit / jarvisCppDocu / jarvisCppSafetyAudit
JCWFs simultaneously, with every ai_call task overridden to use a TestInterface
that returns a fixture stuffed with diverse multi-byte UTF-8 (CJK, emoji,
accented Latin, math, RTL, combining diacritics).  Total: ~420 ai_call tasks
firing heavy UTF-8 reply text into the system in parallel.

Regression armor for the 2026-04-27 truncation-bug class — the TUI ncurses
renderer must tolerate the bytes without crashing, and log/log.txt must
remain well-formed UTF-8 from start to finish of the run.

Verifies:
  - All 3 runs reach a terminal state within timeout.
  - j9t process is still alive after the run (no ncurses crash).
  - log/log.txt bytes appended during the run decode cleanly as UTF-8.

Runs against a Studio Debug build on the default port (8443).  Requires an
admin MCP key via --token or J9T_TOKEN.  Uses TestInterface (no AI cost).
"""

import argparse
import concurrent.futures
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

INTERFACE_NAME = "stress_tui_heavy"
FIXTURE_PATH = h.REPO_ROOT / "test" / "dispatch" / "fixtures" / "api1" / "utf8_heavy.json"
JARVISCPP_WORKFLOWS = ["jarvisCppDocu", "jarvisCppCyberSecAudit", "jarvisCppSafetyAudit"]

# Generous timeout — MockTransport replays fixtures synchronously, so 420
# tasks should complete in well under 5 minutes.  Set the upper bound at 10
# min to absorb disk-write contention without false-flagging slowness.
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

    headers = {"Authorization": f"Bearer {args.token}"}

    if not h.provision_test_interface(args.base_url, headers,
                                        name=INTERFACE_NAME,
                                        fixture_path=FIXTURE_PATH):
        return 1

    log_offset_at_start = h.log_file_size()
    failures = []
    run_ids: dict[str, str] = {}

    try:
        # Build all three JCWFs in advance.
        jcwfs = []
        suffix = time.strftime("%H%M%S")
        for wf_name in JARVISCPP_WORKFLOWS:
            try:
                jcwf = h.load_jarvisCpp_workflow(
                    wf_name,
                    interface_override=INTERFACE_NAME,
                    id_suffix=f"stress_heavy_{suffix}",
                )
                task_count = sum(1 for t in jcwf.get("tasks", {}).values()
                                  if isinstance(t, dict) and t.get("type") == "ai_call")
                print(f"  loaded {wf_name}: {task_count} ai_call tasks "
                      f"(id={jcwf.get('id')})")
                jcwfs.append((wf_name, jcwf))
            except Exception as e:
                failures.append(f"{wf_name}: load failed: {e}")

        if failures:
            return _summarize(failures)

        # Submit all three concurrently.
        print()
        print(f"  submitting 3 workflows concurrently...")
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:
            futures = {
                ex.submit(h.submit_adhoc, args.base_url, headers, jcwf, "on_completion"): wf_name
                for wf_name, jcwf in jcwfs
            }
            for fut in concurrent.futures.as_completed(futures):
                wf_name = futures[fut]
                try:
                    rid = fut.result()
                    if rid:
                        run_ids[wf_name] = rid
                        print(f"    {wf_name} → run {rid}")
                    else:
                        failures.append(f"{wf_name}: submit returned no run id")
                except Exception as e:
                    failures.append(f"{wf_name}: submit raised: {e}")

        if not run_ids:
            return _summarize(failures)

        # Wait for all of them in parallel.
        print()
        print(f"  waiting for {len(run_ids)} runs (timeout {int(RUN_TIMEOUT_S)}s)...")
        t0 = time.time()
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(run_ids)) as ex:
            futures = {
                ex.submit(h.poll_run_state, args.base_url, headers, rid, RUN_TIMEOUT_S): wf_name
                for wf_name, rid in run_ids.items()
            }
            results = {}
            for fut in concurrent.futures.as_completed(futures):
                wf_name = futures[fut]
                try:
                    state = fut.result()
                    results[wf_name] = state
                    print(f"    {wf_name} → {state}")
                    if state not in ("succeeded", "failed"):
                        # cancelled / None (timeout) is unexpected
                        failures.append(f"{wf_name}: terminal state was {state!r}")
                except Exception as e:
                    failures.append(f"{wf_name}: poll raised: {e}")
        elapsed = time.time() - t0
        print(f"  all 3 reached terminal state in {elapsed:.1f}s")

        # Liveness: j9t should still be answering HTTP after the storm.
        if not h.j9t_alive(args.base_url, headers):
            failures.append("j9t process is not alive after the run — likely TUI crash")
        else:
            print(f"  j9t alive (HTTP responding)")

        # Log file invariant: every byte written during the run must decode
        # cleanly as UTF-8.  The truncation/sanitization layer is the only
        # legitimate path; if bad bytes leaked, the renderer / log emitter
        # broke their contract.
        ok, err = h.log_tail_is_valid_utf8(start_offset=log_offset_at_start)
        if not ok:
            failures.append(f"log/log.txt has invalid UTF-8 since the run started: {err}")
        else:
            new_bytes = h.log_file_size() - log_offset_at_start
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
    print("PASS: heavy-UTF-8 + 3-way concurrent jarvisCpp completed without TUI crash, log stays valid UTF-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
