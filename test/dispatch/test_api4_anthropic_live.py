#!/usr/bin/env python3
"""
Live Anthropic API4 round-trip test.

Submits an adhoc ai_call targeting the `api.anthropic.com/claude-opus-4-7/API4`
interface (configured in config.json), waits for the envelope to land through
the HTTP/2 dispatcher + ReplyParserAPI4 path, and asserts that a populated
<prob>.output.txt artifact landed in the run folder.

Gated: costs real Anthropic tokens.  Skip on CI clean installs.

Usage:
    python3 test/dispatch/test_api4_anthropic_live.py
    python3 test/dispatch/test_api4_anthropic_live.py --token mcp_...
    python3 test/dispatch/test_api4_anthropic_live.py --interface api.anthropic.com/claude-opus-4-7/API4
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

DEFAULT_INTERFACE = "api.anthropic.com/claude-opus-4-7/API4"


def build_jcwf(interface_name: str) -> dict:
    return {
        "version": "1.0",
        "id": "adhoc_api4_smoke",
        "label": "Contract: API4 (Anthropic) round-trip",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "ask_opus": {
                "id": "ask_opus",
                "type": "ai_call",
                "label": "Ask Opus something small",
                "mode": "single",
                "working_directory": "../../queue/adhoc_api4_smoke/01_ask_opus",
                "params": {"provider": interface_name},
                "queue_binding": {
                    "stng_files": [{
                        "path": "STNG_x.txt",
                        "content": "Reply with one short sentence. No preamble. No markdown fences.",
                    }],
                    "task_files": [{"path": "TASK_x.txt", "content": "Answer the question."}],
                    "cntx_files": [{"path": "CNTX_x.txt",
                                    "content": "Context: dispatch contract test."}],
                    "prob_files": [{"path": "PROB_x.txt",
                                    "content": "What is 7 plus 4? Reply with just the number."}],
                },
            }
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    parser.add_argument("--interface", default=DEFAULT_INTERFACE)
    parser.add_argument("--timeout-seconds", type=int, default=180,
                        help="Max wait for the run to finish (default 180 s).")
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}

    # Submit
    r = requests.post(
        f"{args.base_url}/api/workflows/run-adhoc",
        json={"jcwf": build_jcwf(args.interface), "cleanup_policy": "ttl_1h"},
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
    print(f"Submitted run: {run_id}")

    # Poll for terminal state
    start = time.time()
    terminal = None
    while time.time() - start < args.timeout_seconds:
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
        time.sleep(1.0)

    if terminal != "succeeded":
        print(f"FAIL: expected 'succeeded' terminal state, got '{terminal}' after {args.timeout_seconds}s")
        return 1

    # List files in the run and find the .output.txt
    rf = requests.get(
        f"{args.base_url}/api/workflow-runs/{run_id}/files",
        headers=headers, verify=False, timeout=30,
    )
    if rf.status_code != 200:
        print(f"FAIL: files listing returned {rf.status_code}")
        return 1
    files = rf.json().get("files", [])
    output_entry = next((f for f in files if f.get("path", "").endswith(".output.txt")), None)
    if output_entry is None:
        print(f"FAIL: no .output.txt in run files ({len(files)} files seen)")
        return 1
    size = output_entry.get("size_bytes", output_entry.get("size", 0))
    if size == 0:
        print(f"FAIL: .output.txt is empty ({output_entry.get('path')})")
        return 1

    # Pull the content and sanity-check it mentions "11"
    rc = requests.get(
        f"{args.base_url}/api/workflow-runs/{run_id}/files/{output_entry['path']}",
        headers=headers, verify=False, timeout=30,
    )
    if rc.status_code != 200:
        print(f"FAIL: file fetch returned {rc.status_code}")
        return 1

    text = rc.text
    if "11" not in text:
        print(f"WARN: reply doesn't contain '11' (but the run succeeded). Reply: {text[:200]!r}")
    else:
        print(f"OK: API4 round-trip succeeded, reply references the expected answer.")
    print(f"Reply (first 200 chars): {text[:200]!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
