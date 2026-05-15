#!/usr/bin/env python3
"""
Hermetic MockTransport contract test.

Configures a transient `is_mock=true` AI interface (api_type=API1) via REST,
submits an adhoc ai_call routed to it, and asserts that the canned JSON
fixture's `content` field lands byte-exact in `<prob>.output.txt`.  Zero
network calls — MockTransport replays the fixture body through the real
ReplyParserAPI1, exercising the dispatcher's full code path (AIMD admission,
retry queue, parser dispatch).

Replaces the pre-Sitting-2 `test_testinterface_hermetic.py` which exercised
the now-removed TestInterface short-circuit.  See Foundation Sitting 2 of
`doc/misc/ai-provider-error-visibility-dev-plan.md`.

Runs against a live JarvisAgent instance (default https://localhost:8443).
Requires an MCP admin key via --token or the J9T_TOKEN env var.
"""

import argparse
import json
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

MOCK_INTERFACE_NAME = "hermetic_mock_dispatch"
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_PATH_DEFAULT = os.path.join(REPO_ROOT, "test", "dispatch", "fixtures", "api1", "golden_success.json")


def build_jcwf(interface_name: str) -> dict:
    return {
        "version": "1.0",
        "id": "adhoc_hermetic_mock_dispatch",
        "label": "Contract: hermetic MockTransport round-trip",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "echo": {
                "id": "echo",
                "type": "ai_call",
                "label": "Hermetic dispatch",
                "mode": "single",
                "working_directory": "../../queue/adhoc_hermetic_mock_dispatch/01_echo",
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
                        {"path": "PROB_x.txt", "content": "Anything here — MockTransport ignores it."}
                    ],
                },
            }
        },
    }


def create_mock_interface(base_url: str, headers: dict, fixture_path: str) -> bool:
    body = {
        "name": MOCK_INTERFACE_NAME,
        "description": "Hermetic MockTransport interface for dispatch contract tests",
        "url": "https://localhost/_mock_/never_called",
        "model": "mock-stub",
        "api_type": "API1",
        "key_name": "",
        "is_mock": True,
        "fixture_path": fixture_path,
    }
    r = requests.post(
        f"{base_url}/api/settings/ai-interfaces",
        json=body, headers=headers, verify=False, timeout=10,
    )
    if r.status_code not in (200, 201):
        print(f"FAIL: create mock interface returned {r.status_code}: {r.text[:200]}")
        return False
    return True


def delete_mock_interface(base_url: str, headers: dict) -> None:
    try:
        requests.delete(
            f"{base_url}/api/settings/ai-interfaces/{MOCK_INTERFACE_NAME}",
            headers=headers, verify=False, timeout=10,
        )
    except Exception:
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
                        help="Absolute path to the JSON fixture (OpenAI chat completion shape).")
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1
    if not os.path.isfile(args.fixture_path):
        print(f"FAIL: fixture file not found at {args.fixture_path}")
        return 1

    # The parser extracts choices[0].message.content from the JSON fixture and
    # writes it (verbatim, byte-exact) to <prob>.output.txt.  Reading the JSON
    # here means the test stays correct if the fixture's content string changes.
    with open(args.fixture_path, "r", encoding="utf-8") as f:
        fixture_doc = json.load(f)
    expected_content = fixture_doc["choices"][0]["message"]["content"]
    expected_bytes = expected_content.encode("utf-8")

    headers = {"Authorization": f"Bearer {args.token}"}

    if not create_mock_interface(args.base_url, headers, args.fixture_path):
        return 1

    try:
        # ttl_1h (the shortest retention) instead of on_completion — the mock
        # backend writes the output and transitions to succeeded in the same
        # tick, which races the on_completion reaper and can wipe the folder
        # before the file-listing call lands.
        r = requests.post(
            f"{args.base_url}/api/workflows/run-adhoc",
            json={"jcwf": build_jcwf(MOCK_INTERFACE_NAME), "cleanup_policy": "ttl_1h"},
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
        # FileWriter prepends a "# Model: …" header line; strip it before
        # byte-comparing against the fixture content.  See aiRequestPool's
        # WriteWithHeader / OutputWriter for the exact header format.
        if actual_bytes.startswith(b"# Model:"):
            nl = actual_bytes.find(b"\n")
            if nl != -1:
                actual_bytes = actual_bytes[nl + 1:]
        if actual_bytes != expected_bytes:
            print("FAIL: .output.txt bytes differ from the fixture content field.")
            print(f"     expected ({len(expected_bytes)}b): {expected_bytes!r}")
            print(f"     actual   ({len(actual_bytes)}b): {actual_bytes!r}")
            return 1

        # PROV sidecar: the executor writes it per dispatch as a debug/replay
        # record.  When mocked, it carries mocked: true + fixture_path so
        # operators can distinguish mock dispatches from live ones in
        # post-mortem.  See Sitting 2 of the dev plan §4 Foundation.
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
        if MOCK_INTERFACE_NAME not in prov_text:
            print(f"FAIL: PROV sidecar does not reference interface '{MOCK_INTERFACE_NAME}'")
            print(f"     content: {prov_text[:300]!r}")
            return 1
        if '"mocked": true' not in prov_text and '"mocked":true' not in prov_text:
            print(f"FAIL: PROV sidecar missing mocked: true field (Sitting 2 requirement)")
            print(f"     content: {prov_text[:500]!r}")
            return 1

        print(f"OK: hermetic MockTransport dispatched end-to-end; "
              f".output.txt matches fixture content field ({len(expected_bytes)} bytes); "
              f"PROV sidecar records mocked=true + interface='{MOCK_INTERFACE_NAME}'.")
        return 0

    finally:
        delete_mock_interface(args.base_url, headers)


if __name__ == "__main__":
    sys.exit(main())
