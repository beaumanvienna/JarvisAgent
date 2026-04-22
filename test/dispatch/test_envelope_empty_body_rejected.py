#!/usr/bin/env python3
"""
Contract test: an ai_call with an entirely-whitespace body is rejected.

Adhoc-submits a minimal JCWF whose STNG/TASK/CNTX/PROB are all empty or
whitespace.  AiRequestPool::Submit is expected to reject the envelope before
any HTTP call; the task should transition to Failed.

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


EMPTY_BODY_JCWF = {
    "version": "1.0",
    "id": "adhoc_empty_body",
    "label": "Contract: empty ai_call body rejection",
    "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
    "tasks": {
        "gen": {
            "id": "gen",
            "type": "ai_call",
            "label": "AI call with empty prompt",
            "mode": "single",
            "working_directory": "../../queue/adhoc_empty_body/01_gen",
            "queue_binding": {
                "stng_files": [{"path": "STNG_x.txt", "content": "   \n  \n"}],
                "task_files": [{"path": "TASK_x.txt", "content": ""}],
                "cntx_files": [{"path": "CNTX_x.txt", "content": ""}],
                "prob_files": [{"path": "PROB_x.txt", "content": "   \n\n"}],
            },
        }
    },
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

    # Submit
    r = requests.post(
        f"{args.base_url}/api/workflows/run-adhoc",
        json={"jcwf": EMPTY_BODY_JCWF, "cleanup_policy": "on_completion"},
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

    # Poll for terminal state
    terminal = None
    for _ in range(60):
        rs = requests.get(
            f"{args.base_url}/api/workflow-runs/{run_id}",
            headers=headers, verify=False, timeout=30,
        )
        if rs.status_code == 200:
            body = rs.json()
            run = body.get("run") if isinstance(body.get("run"), dict) else body
            state = run.get("state")
            if state in ("succeeded", "failed", "cancelled"):
                terminal = state
                break
        time.sleep(0.5)

    if terminal != "failed":
        print(f"FAIL: expected terminal state 'failed' but got '{terminal}'")
        return 1

    print("OK: empty-body ai_call was rejected (run state = failed).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
