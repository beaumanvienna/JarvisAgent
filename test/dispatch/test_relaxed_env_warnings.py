#!/usr/bin/env python3
"""
Relaxed-env contract test.

Phase 1 of the AI dispatch refactor relaxed the env-file rule: STNG / CNTX /
TASK are optional; PROB remains required; dispatch proceeds as long as the
combined body has >= 1 non-whitespace character (see `AI dispatch
refactor.md` §1 decision +1 and §8 Phase 1).  Warnings are logged for missing
categories but the envelope still goes through.

Uses the hermetic Test interface so the assertion is hermetic and
network-free: if Submit rejected the envelope, the run would end Failed;
if Submit accepted it, the Test path writes `<prob>.output.txt` byte-exact
from the fixture and the run ends Succeeded.

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

TEST_INTERFACE_NAME = "relaxed_env_test_dispatch"
FIXTURE_PATH_DEFAULT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "fixtures", "api1", "golden_success.json")
)


def build_jcwf(case_name: str, include_stng: bool, include_task: bool,
               include_cntx: bool) -> dict:
    queue_binding = {
        "prob_files": [{"path": "PROB_x.txt",
                        "content": "Dispatch me — the Test interface ignores the prompt."}],
    }
    if include_stng:
        queue_binding["stng_files"] = [{"path": "STNG_x.txt", "content": "settings."}]
    if include_task:
        queue_binding["task_files"] = [{"path": "TASK_x.txt", "content": "task."}]
    if include_cntx:
        queue_binding["cntx_files"] = [{"path": "CNTX_x.txt", "content": "context."}]

    wf_id = f"adhoc_relaxed_{case_name}"
    return {
        "version": "1.0",
        "id": wf_id,
        "label": f"Contract: relaxed env — {case_name}",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "dispatch": {
                "id": "dispatch",
                "type": "ai_call",
                "label": f"Dispatch ({case_name})",
                "mode": "single",
                "working_directory": f"../../queue/{wf_id}/01_dispatch",
                "params": {"provider": TEST_INTERFACE_NAME},
                "queue_binding": queue_binding,
            }
        },
    }


def create_test_interface(base_url: str, headers: dict, fixture_path: str) -> bool:
    r = requests.post(
        f"{base_url}/api/settings/ai-interfaces",
        json={
            "name": TEST_INTERFACE_NAME,
            "description": "Relaxed-env MockTransport interface",
            "url": "https://localhost/_mock_/never_called",
            "model": "mock-stub",
            "api_type": "API1",
            "key_name": "",
            "is_mock": True,
            "fixture_path": fixture_path,
        },
        headers=headers, verify=False, timeout=10,
    )
    return r.status_code in (200, 201)


def delete_test_interface(base_url: str, headers: dict) -> None:
    try:
        requests.delete(
            f"{base_url}/api/settings/ai-interfaces/{TEST_INTERFACE_NAME}",
            headers=headers, verify=False, timeout=10,
        )
    except Exception:
        pass


def run_case(base_url: str, headers: dict, case_name: str, **flags) -> str | None:
    """Submit one adhoc JCWF, return terminal state (or None on timeout)."""
    # ttl_1h so artifacts survive the polling + file-listing assertions.
    r = requests.post(
        f"{base_url}/api/workflows/run-adhoc",
        json={"jcwf": build_jcwf(case_name, **flags), "cleanup_policy": "ttl_1h"},
        headers=headers, verify=False, timeout=30,
    )
    if r.status_code not in (200, 202):
        print(f"FAIL [{case_name}]: run-adhoc returned {r.status_code}: {r.text[:200]}")
        return None

    run_id = r.json().get("runId") or r.json().get("run_id")
    if not run_id:
        return None

    start = time.time()
    while time.time() - start < 30:
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


CASES = [
    # (case_name, include_stng, include_task, include_cntx)
    ("all_four_present", True, True, True),       # baseline — pre-refactor behaviour
    ("no_stng", False, True, True),                # missing STNG alone
    ("no_task", True, False, True),                # missing TASK alone
    ("no_cntx", True, True, False),                # missing CNTX alone
    ("only_prob", False, False, False),            # PROB alone — the strongest relaxation
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    parser.add_argument("--fixture-path", default=FIXTURE_PATH_DEFAULT)
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    if not os.path.isfile(args.fixture_path):
        print(f"FAIL: fixture file not found at {args.fixture_path}")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}

    if not create_test_interface(args.base_url, headers, args.fixture_path):
        print("FAIL: could not create Test interface")
        return 1

    try:
        failures = []
        for case_name, stng, task, cntx in CASES:
            state = run_case(args.base_url, headers, case_name,
                             include_stng=stng, include_task=task, include_cntx=cntx)
            if state == "succeeded":
                print(f"OK [{case_name}]: dispatched + succeeded (STNG={stng}, TASK={task}, CNTX={cntx})")
            else:
                failures.append((case_name, state))
                print(f"FAIL [{case_name}]: expected 'succeeded', got '{state}'")

        if failures:
            print(f"\n{len(failures)}/{len(CASES)} cases failed.")
            return 1
        print(f"\nOK: all {len(CASES)} relaxed-env cases dispatched cleanly.")
        return 0
    finally:
        delete_test_interface(args.base_url, headers)


if __name__ == "__main__":
    sys.exit(main())
