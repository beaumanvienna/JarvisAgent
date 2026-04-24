#!/usr/bin/env python3
"""
Hermetic TestInterface contract test.

Configures a transient `InterfaceType::Test` AI interface via REST, submits
an adhoc ai_call routed to it, and asserts that the canned fixture reply
lands byte-exact in `<prob>.output.txt`.  Zero network calls — the Test
interface short-circuits the curl dispatcher (see aiRequestPool.cpp §984).

Covers the Phase 7 hermetic-mode requirement from
`AI dispatch refactor.md` §8 and the refactor handoff's open follow-up
"Automated hermetic test driving InterfaceType::Test".

Leaves the in-memory config with the Test interface present (no /save call),
and DELETEs it on exit so nothing persists to disk.

Runs against a live JarvisAgent instance (default https://localhost:8443).
Requires an MCP admin key via --token or the J9T_TOKEN env var.
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

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

TEST_INTERFACE_NAME = "hermetic_test_dispatch"
FIXTURE_PATH_DEFAULT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "fixtures", "hermetic_reply.txt")
)


def build_jcwf(interface_name: str) -> dict:
    return {
        "version": "1.0",
        "id": "adhoc_hermetic_dispatch",
        "label": "Contract: hermetic Test interface round-trip",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "echo": {
                "id": "echo",
                "type": "ai_call",
                "label": "Hermetic dispatch",
                "mode": "single",
                "working_directory": "../../queue/adhoc_hermetic_dispatch/01_echo",
                "params": {"provider": interface_name},
                "queue_binding": {
                    "stng_files": [
                        {"path": "STNG_x.txt", "content": "Hermetic dispatch test."}
                    ],
                    "task_files": [
                        {"path": "TASK_x.txt", "content": "No-op — reply comes from the fixture."}
                    ],
                    "cntx_files": [
                        {"path": "CNTX_x.txt", "content": "Canned reply test."}
                    ],
                    "prob_files": [
                        {"path": "PROB_x.txt", "content": "Anything here — the Test interface ignores it."}
                    ],
                },
            }
        },
    }


def create_test_interface(base_url: str, headers: dict, fixture_path: str) -> bool:
    body = {
        "name": TEST_INTERFACE_NAME,
        "description": "Hermetic Test interface for dispatch contract tests",
        "url": fixture_path,
        "model": "hermetic-stub",
        "api_type": "Test",
        "key_name": "",
    }
    r = requests.post(
        f"{base_url}/api/settings/ai-interfaces",
        json=body, headers=headers, verify=False, timeout=10,
    )
    if r.status_code not in (200, 201):
        print(f"FAIL: create Test interface returned {r.status_code}: {r.text[:200]}")
        return False
    return True


def delete_test_interface(base_url: str, headers: dict) -> None:
    try:
        requests.delete(
            f"{base_url}/api/settings/ai-interfaces/{TEST_INTERFACE_NAME}",
            headers=headers, verify=False, timeout=10,
        )
    except Exception:
        # Best effort — the test's primary job was verifying dispatch, not teardown.
        pass


def poll_run_state(base_url: str, headers: dict, run_id: str, timeout_s: int = 30) -> str | None:
    start = time.time()
    while time.time() - start < timeout_s:
        rs = requests.get(
            f"{base_url}/api/workflow-runs/{run_id}",
            headers=headers, verify=False, timeout=10,
        )
        if rs.status_code == 200:
            body = rs.json()
            run = body.get("run") if isinstance(body.get("run"), dict) else body
            state = run.get("state")
            if state in ("succeeded", "failed", "cancelled"):
                return state
        time.sleep(0.3)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    parser.add_argument("--fixture-path", default=FIXTURE_PATH_DEFAULT,
                        help="Absolute path to the canned-reply fixture the Test interface loads.")
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    if not os.path.isfile(args.fixture_path):
        print(f"FAIL: fixture file not found at {args.fixture_path}")
        return 1

    with open(args.fixture_path, "rb") as f:
        expected_bytes = f.read()

    headers = {"Authorization": f"Bearer {args.token}"}

    if not create_test_interface(args.base_url, headers, args.fixture_path):
        return 1

    try:
        # ttl_1h (the shortest retention) instead of on_completion — the Test
        # backend writes the output and transitions to succeeded in the same
        # tick, which races the on_completion reaper and can wipe the folder
        # before the file-listing call lands.
        r = requests.post(
            f"{args.base_url}/api/workflows/run-adhoc",
            json={"jcwf": build_jcwf(TEST_INTERFACE_NAME), "cleanup_policy": "ttl_1h"},
            headers=headers, verify=False, timeout=30,
        )
        if r.status_code not in (200, 202):
            print(f"FAIL: run-adhoc returned {r.status_code}: {r.text[:200]}")
            return 1
        payload = r.json()
        run_id = payload.get("runId") or payload.get("run_id")
        if not run_id:
            print(f"FAIL: no runId in response: {r.text[:200]}")
            return 1

        terminal = poll_run_state(args.base_url, headers, run_id)
        if terminal != "succeeded":
            print(f"FAIL: expected terminal state 'succeeded' but got '{terminal}'")
            return 1

        # on_completion cleanup may wipe the folder the instant the run ends;
        # fetch files immediately via list_run_files.  The Test path also
        # writes <prob>.output.txt before OnOutputFileCreated fires, so it
        # should appear even if the cleanup racing begins in parallel.
        rf = requests.get(
            f"{args.base_url}/api/workflow-runs/{run_id}/files",
            headers=headers, verify=False, timeout=10,
        )
        if rf.status_code != 200:
            print(f"FAIL: files listing returned {rf.status_code}")
            return 1
        files = rf.json().get("files", [])
        output_entry = next((f for f in files if f.get("path", "").endswith(".output.txt")), None)
        if output_entry is None:
            print(f"FAIL: no .output.txt among {len(files)} run files")
            return 1

        rc = requests.get(
            f"{args.base_url}/api/workflow-runs/{run_id}/files/{output_entry['path']}",
            headers=headers, verify=False, timeout=10,
        )
        if rc.status_code != 200:
            print(f"FAIL: file fetch returned {rc.status_code}")
            return 1

        actual_bytes = rc.content
        if actual_bytes != expected_bytes:
            print("FAIL: .output.txt bytes differ from the fixture.")
            print(f"     expected ({len(expected_bytes)}b): {expected_bytes!r}")
            print(f"     actual   ({len(actual_bytes)}b): {actual_bytes!r}")
            return 1

        # PROV sidecar: the executor writes it per dispatch as a debug/replay
        # record.  Per refactor §3 it's write-only (never consumed at runtime),
        # but its presence + `api_type:"Test"` lines up with the envelope and
        # proves the PROV pipeline wasn't accidentally lost with SessionManager.
        prov_entry = next((f for f in files if f.get("path", "").startswith("PROV_")
                           or "/PROV_" in f.get("path", "")), None)
        if prov_entry is None:
            print("FAIL: no PROV_ sidecar among run files — expected one per ai_call dispatch")
            return 1

        rp = requests.get(
            f"{args.base_url}/api/workflow-runs/{run_id}/files/{prov_entry['path']}",
            headers=headers, verify=False, timeout=10,
        )
        if rp.status_code != 200:
            print(f"FAIL: PROV fetch returned {rp.status_code}")
            return 1
        prov_text = rp.text
        if TEST_INTERFACE_NAME not in prov_text:
            print(f"FAIL: PROV sidecar does not reference interface '{TEST_INTERFACE_NAME}'")
            print(f"     content: {prov_text[:300]!r}")
            return 1
        # PROV currently records provider / url / model only (not api_type).

        print(f"OK: hermetic Test interface dispatched end-to-end; "
              f".output.txt matches fixture ({len(expected_bytes)} bytes); "
              f"PROV sidecar records interface='{TEST_INTERFACE_NAME}'.")
        return 0

    finally:
        delete_test_interface(args.base_url, headers)


if __name__ == "__main__":
    sys.exit(main())
