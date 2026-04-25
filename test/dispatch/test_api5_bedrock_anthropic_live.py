#!/usr/bin/env python3
"""
Live Bedrock-on-Anthropic API5 round-trip test.

Submits an adhoc ai_call targeting an API5 (AWS Bedrock) interface configured for an
Anthropic Claude model. Use against either LocalStack (free Hobby tier; local Ollama-backed
small models) or a real AWS Bedrock account.

Prerequisites in j9t:
- An interface in config.json with API="API5" and url like
    http://localhost:4566                        (LocalStack)
    https://bedrock-runtime.us-east-1.amazonaws.com   (real AWS)
  Model is the full Bedrock modelId, e.g. "anthropic.claude-3-haiku-20240307-v1:0".
- A provider entry in keys.json.enc with credential_type="aws":
    api_key  = AWS access_key_id
    params:
      secret_access_key: <40-char secret>
      region:            us-east-1
      session_token:     (optional STS token)

Setting up LocalStack with Bedrock for local testing (free Hobby tier, signup required):

  1. Sign up at https://app.localstack.cloud/  (Hobby tier, free for non-commercial / OSS).
  2. Save the auth token (gitignored): mkdir -p secrets && chmod 700 secrets
                                       echo "<token>" > secrets/localstack_token
                                       chmod 600 secrets/localstack_token
  3. docker run --rm -d --name localstack \\
         -p 4566:4566 \\
         -e SERVICES=bedrock,bedrock-runtime \\
         -e LOCALSTACK_AUTH_TOKEN="$(cat secrets/localstack_token)" \\
         -v /var/run/docker.sock:/var/run/docker.sock \\
         localstack/localstack:latest

Both `bedrock` AND `bedrock-runtime` need to be enabled — the runtime hosts the /invoke
endpoint. First request triggers an Ollama model pull and can take several minutes. AWS
SigV4 dummy credentials work (LocalStack accepts the signature shape but doesn't verify).

Usage:
    python3 test/dispatch/test_api5_bedrock_anthropic_live.py
    python3 test/dispatch/test_api5_bedrock_anthropic_live.py --token mcp_...
    python3 test/dispatch/test_api5_bedrock_anthropic_live.py --interface bedrock-localstack/anthropic.claude-3-haiku-20240307-v1:0/API5
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

DEFAULT_INTERFACE = "bedrock-localstack/anthropic.claude-3-haiku-20240307-v1:0/API5"


def build_jcwf(interface_name: str) -> dict:
    return {
        "version": "1.0",
        "id": "adhoc_api5_anthropic_smoke",
        "label": "Contract: API5 (Bedrock + Anthropic) round-trip",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "ask_bedrock": {
                "id": "ask_bedrock",
                "type": "ai_call",
                "label": "Ask Bedrock-hosted Claude something small",
                "mode": "single",
                "working_directory": "../../queue/adhoc_api5_anthropic_smoke/01_ask_bedrock",
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
    parser.add_argument("--timeout-seconds", type=int, default=300,
                        help="Max wait for the run to finish (default 300 s — LocalStack first-run is slow).")
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}

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

    rc = requests.get(
        f"{args.base_url}/api/workflow-runs/{run_id}/files/{output_entry['path']}",
        headers=headers, verify=False, timeout=30,
    )
    if rc.status_code != 200:
        print(f"FAIL: file fetch returned {rc.status_code}")
        return 1

    text = rc.text
    print(f"OK: API5 (Bedrock+Anthropic) round-trip succeeded.")
    print(f"Reply (first 200 chars): {text[:200]!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
