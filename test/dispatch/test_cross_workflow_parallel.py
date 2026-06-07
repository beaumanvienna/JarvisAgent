#!/usr/bin/env python3
"""
Regression test for the cross-workflow parallel concurrency bug (2026-04-22).

The bug: `WorkflowRuntimeManager::TickActiveRun` captured `&workflowDefinition`
(a reference into `std::vector<ActiveRun> m_ActiveRuns`) into its thread-pool
lambda. When a later tick pushed a new run, `vector::push_back` reallocated
and the in-flight lambda's reference dangled — workers then resolved task
paths against the WRONG workflow's base directory. Symptom: a task's output
file (e.g. `response.json`) landed inside an unrelated workflow's folder.
Observed on 2026-04-22 with `jiraIssueDemo` scattering its `response.json`
into `workflows/cyber2/jiraIssueDemo/02_create/`.

Fix: `m_ActiveRuns` switched to `std::vector<std::unique_ptr<ActiveRun>>`
so element addresses stay stable across push_back. This test locks down the
class of bug by firing N adhoc runs simultaneously with distinct queue-folder
paths + unique PROB filenames, then asserting each run's output file lands
exactly where it should.

Approach
--------
  * N (default 12) adhoc JCWFs submitted as fast as possible via
    `POST /api/workflows/run-adhoc`. Each JCWF uses a unique `id`,
    a unique PROB filename (`PROB_<runIndex>.txt`) and routes through
    a hermetic `is_mock: true` interface (MockTransport replay) so
    the test is offline.
  * After all runs reach a terminal state, for every run we fetch
    `/api/workflow-runs/{run_id}/files` and verify:
      - the run succeeded,
      - exactly one `.output.txt` is present (no cross-contamination),
      - its filename matches this run's own unique PROB prefix, and
      - its bytes match the fixture's parsed `content` field byte-exact
        (after stripping FileWriter's `# Model: …` header line).

If the concurrency bug regresses, a later run's push_back dangles an
earlier lambda's reference and that lambda ends up resolving its output
path against the new run's base directory. The earlier run's file
either never lands in its own folder, or the wrong run's PROB name
shows up — both caught by the assertions above.

Runs against a live JarvisAgent instance (default https://localhost:8443).
Requires an MCP admin key via --token or the J9T_TOKEN env var.
"""

import argparse
import concurrent.futures
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _provisioning  # noqa: E402

TEST_INTERFACE_NAME = "hermetic_crossparallel"
FIXTURE_PATH_DEFAULT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "fixtures", "api1", "golden_success.json")
)


def build_jcwf(run_index: int, interface_name: str) -> dict:
    jcwf_id = f"adhoc_crossparallel_{run_index:02d}"
    prob_stem = f"PROB_{run_index:02d}"
    return {
        "version": "1.0",
        "id": jcwf_id,
        "label": f"Contract: cross-workflow parallel run {run_index:02d}",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "echo": {
                "id": "echo",
                "type": "ai_call",
                "label": f"Hermetic dispatch {run_index:02d}",
                "mode": "single",
                "working_directory": f"../../queue/{jcwf_id}/01_echo",
                "params": {"provider": interface_name},
                "queue_binding": {
                    "stng_files": [{"path": "STNG_x.txt", "content": "Cross-parallel concurrency test."}],
                    "task_files": [{"path": "TASK_x.txt", "content": "No-op — reply is canned."}],
                    "cntx_files": [{"path": "CNTX_x.txt", "content": "Hermetic."}],
                    # Unique PROB name per run — surfaces cross-contamination
                    # immediately: if an earlier run's lambda read a later run's
                    # base directory, the PROB filename in the output listing
                    # won't match this run's index.
                    "prob_files": [{"path": f"{prob_stem}.txt",
                                    "content": f"run={run_index:02d}"}],
                },
            }
        },
    }


def create_test_interface(base_url: str, headers: dict, fixture_path: str) -> bool:
    body = {
        "name": TEST_INTERFACE_NAME,
        "description": "Hermetic MockTransport interface for cross-parallel dispatch regression",
        "url": "https://localhost/_mock_/never_called",
        "model": "mock-stub",
        "api_type": "API1",
        "key_name": "",
        "is_mock": True,
        "fixture_path": fixture_path,
    }
    r = _provisioning.create_interface(base_url, headers, body)
    if r.status_code not in (200, 201):
        print(f"FAIL: create Test interface returned {r.status_code}: {r.text[:200]}")
        return False
    return True


def delete_test_interface(base_url: str, headers: dict) -> None:
    try:
        _provisioning.delete_interface(base_url, headers, TEST_INTERFACE_NAME)
    except Exception:
        pass


def submit_adhoc(base_url: str, headers: dict, jcwf: dict) -> str | None:
    r = requests.post(
        f"{base_url}/api/workflows/run-adhoc",
        json={"jcwf": jcwf, "cleanup_policy": "ttl_1h"},
        headers=headers, verify=False, timeout=30,
    )
    if r.status_code not in (200, 202):
        print(f"FAIL: run-adhoc for {jcwf['id']} returned {r.status_code}: {r.text[:200]}")
        return None
    payload = r.json()
    run_id = payload.get("runId") or payload.get("run_id")
    if not run_id:
        print(f"FAIL: run-adhoc for {jcwf['id']} returned no runId: {r.text[:200]}")
    return run_id


def poll_run_state(base_url: str, headers: dict, run_id: str, timeout_s: int = 60) -> str | None:
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
                        help="Absolute path to the canned-reply fixture.")
    parser.add_argument("--count", type=int, default=12,
                        help="Number of parallel adhoc runs to fire. Larger counts widen "
                             "the reallocation window; 12 reliably triggers several "
                             "vector::push_back events on a stock laptop.")
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    if not os.path.isfile(args.fixture_path):
        print(f"FAIL: fixture file not found at {args.fixture_path}")
        return 1

    # Extract the parsed-content bytes from the JSON fixture — that's what
    # ReplyParserAPI1 will write to <prob>.output.txt (post-MockTransport
    # migration; the fixture is now a full OpenAI chat completion response).
    with open(args.fixture_path, "r", encoding="utf-8") as f:
        fixture_doc = json.load(f)
    expected_bytes = fixture_doc["choices"][0]["message"]["content"].encode("utf-8")

    headers = {"Authorization": f"Bearer {args.token}"}

    if not create_test_interface(args.base_url, headers, args.fixture_path):
        return 1

    try:
        # Build every payload up-front, then fire all submissions in parallel
        # threads so the server sees a near-simultaneous burst. That's what
        # forces repeated vector::push_back into m_ActiveRuns while earlier
        # runs are still in-flight — the exact reallocation window the
        # concurrency bug lived in.
        jcwfs = [build_jcwf(i, TEST_INTERFACE_NAME) for i in range(args.count)]

        run_ids: list[tuple[int, str]] = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.count) as pool:
            futures = {pool.submit(submit_adhoc, args.base_url, headers, j): idx
                       for idx, j in enumerate(jcwfs)}
            for fut in concurrent.futures.as_completed(futures):
                idx = futures[fut]
                run_id = fut.result()
                if run_id is None:
                    return 1
                run_ids.append((idx, run_id))

        print(f"OK: submitted {len(run_ids)} adhoc runs in parallel")

        # Wait for every run to terminate.
        for idx, run_id in run_ids:
            state = poll_run_state(args.base_url, headers, run_id)
            if state != "succeeded":
                print(f"FAIL: run {idx:02d} ({run_id}) ended in state {state!r} "
                      f"(expected 'succeeded'). Cross-workflow push_back may have "
                      f"dangled a reference mid-dispatch — the concurrency bug "
                      f"from 2026-04-22 has regressed.")
                return 1

        print(f"OK: all {len(run_ids)} runs reached 'succeeded'")

        # Verify each run's output landed in ITS OWN folder with ITS OWN PROB stem.
        # A regression would show one of:
        #   - a run has no .output.txt (its lambda wrote to another run's folder), or
        #   - a run's .output.txt filename carries the wrong PROB index (content
        #     resolved against the wrong workflow's base directory).
        for idx, run_id in run_ids:
            expected_stem = f"PROB_{idx:02d}"
            rf = requests.get(
                f"{args.base_url}/api/workflow-runs/{run_id}/files",
                headers=headers, verify=False, timeout=10,
            )
            if rf.status_code != 200:
                print(f"FAIL: files listing for run {idx:02d} ({run_id}) returned {rf.status_code}")
                return 1
            files = rf.json().get("files", [])
            output_entries = [f for f in files if f.get("path", "").endswith(".output.txt")]
            if len(output_entries) != 1:
                print(f"FAIL: run {idx:02d} expected exactly 1 .output.txt, found {len(output_entries)}: "
                      f"{[f.get('path') for f in output_entries]}")
                return 1
            output_path = output_entries[0]["path"]
            output_basename = os.path.basename(output_path)
            if not output_basename.startswith(expected_stem):
                print(f"FAIL: run {idx:02d} output filename is '{output_basename}', expected it to "
                      f"start with '{expected_stem}'. Cross-workflow contamination — a different run's "
                      f"PROB name landed in this run's folder. Concurrency bug likely regressed.")
                return 1

            rc = requests.get(
                f"{args.base_url}/api/workflow-runs/{run_id}/files/{output_path}",
                headers=headers, verify=False, timeout=10,
            )
            if rc.status_code != 200:
                print(f"FAIL: output fetch for run {idx:02d} returned {rc.status_code}")
                return 1
            # FileWriter prepends a "# Model: …\n" header line before the
            # parsed reply.  Strip it before byte-comparing against the
            # fixture's content field.
            actual_bytes = rc.content
            if actual_bytes.startswith(b"# Model:"):
                nl = actual_bytes.find(b"\n")
                if nl != -1:
                    actual_bytes = actual_bytes[nl + 1:]
            if actual_bytes != expected_bytes:
                print(f"FAIL: run {idx:02d} output bytes differ from fixture content "
                      f"({len(actual_bytes)}b vs {len(expected_bytes)}b expected)")
                return 1

        print(f"OK: every run's output landed in its own folder with its own PROB stem "
              f"(byte-exact match across {len(run_ids)} runs) — concurrency fix holds.")
        return 0

    finally:
        delete_test_interface(args.base_url, headers)


if __name__ == "__main__":
    sys.exit(main())
