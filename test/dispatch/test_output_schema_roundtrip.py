#!/usr/bin/env python3
"""
Live-AI contract test: ai_call with `output_schema` produces a valid
`<prob>.output.json` on the happy path.

Covers Phase 3 of the AI dispatch refactor end-to-end:
  - The executor sets `AiInvocation.m_OutputSchemaJson` / `m_StructuredMode`.
  - The provider returns JSON that conforms to the schema.
  - The reply path writes `<prob>.output.json` (not `.output.txt`).
  - The schema validator accepts the reply on first attempt (no retries
    consumed against the `output_retries` budget).

If the provider *does* need a retry (rare for simple schemas on strong
models), the test still passes as long as the final reply validates and the
total dispatch count stays within the budget; we check the schema-retry
counter against a conservative upper bound.

Usage:
    export J9T_TOKEN=mcp_...
    python3 test/dispatch/test_output_schema_roundtrip.py
    python3 test/dispatch/test_output_schema_roundtrip.py --interface api.openai.com/gpt-4.1-mini/API1
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

DEFAULT_INTERFACE = "api.openai.com/gpt-4.1-mini/API1"

OUTPUT_SCHEMA = {
    "type": "object",
    "properties": {
        "answer": {"type": "integer"},
        "unit": {"type": "string", "enum": ["count", "km", "kg"]},
    },
    "required": ["answer", "unit"],
    "additionalProperties": False,
}


def build_jcwf(interface_name: str) -> dict:
    return {
        "version": "1.0",
        "id": "adhoc_schema_roundtrip",
        "label": "Contract: output_schema roundtrip",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "ask_structured": {
                "id": "ask_structured",
                "type": "ai_call",
                "label": "Ask structured",
                "mode": "single",
                "working_directory": "../../queue/adhoc_schema_roundtrip/01_ask_structured",
                "params": {"provider": interface_name},
                "output_schema": OUTPUT_SCHEMA,
                "output_retries": 3,
                "queue_binding": {
                    "stng_files": [{"path": "STNG_x.txt",
                                    "content": "Answer with only a JSON object. No preamble, no markdown fences."}],
                    "task_files": [{"path": "TASK_x.txt",
                                    "content": "Produce a JSON object conforming to the declared output schema."}],
                    "cntx_files": [{"path": "CNTX_x.txt",
                                    "content": "Context: contract test; pick any plausible value for unit."}],
                    "prob_files": [{"path": "PROB_x.txt",
                                    "content": "How many sides does a hexagon have?  Return {answer: <int>, unit: 'count'}."}],
                },
            }
        },
    }


def poll_run_state(base_url: str, headers: dict, run_id: str, timeout_s: int = 180) -> str | None:
    start = time.time()
    while time.time() - start < timeout_s:
        rs = requests.get(
            f"{base_url}/api/workflow-runs/{run_id}",
            headers=headers, verify=False, timeout=30,
        )
        if rs.status_code == 200:
            body = rs.json()
            run = body.get("run") if isinstance(body.get("run"), dict) else body
            state = run.get("state")
            if state in ("succeeded", "failed", "cancelled"):
                return state
        time.sleep(1.0)
    return None


def fetch_signals(base_url: str, headers: dict) -> dict:
    r = requests.get(f"{base_url}/api/debug/signals",
                     headers=headers, verify=False, timeout=10)
    if r.status_code != 200:
        return {}
    return r.json().get("signals", r.json())


def validate_schema(data: object, schema: dict) -> str | None:
    """Tiny subset validator sufficient for OUTPUT_SCHEMA above.  Returns an
    error string on failure, None on success."""
    if schema.get("type") == "object":
        if not isinstance(data, dict):
            return f"expected object, got {type(data).__name__}"
        for required in schema.get("required", []):
            if required not in data:
                return f"missing required field '{required}'"
        props = schema.get("properties", {})
        for k, v in data.items():
            if schema.get("additionalProperties") is False and k not in props:
                return f"unexpected additional property '{k}'"
            sub = props.get(k)
            if sub:
                err = validate_schema(v, sub)
                if err:
                    return f"{k}: {err}"
        return None
    if schema.get("type") == "integer":
        if not isinstance(data, int) or isinstance(data, bool):
            return f"expected integer, got {type(data).__name__}"
        return None
    if schema.get("type") == "string":
        if not isinstance(data, str):
            return f"expected string, got {type(data).__name__}"
        if "enum" in schema and data not in schema["enum"]:
            return f"value '{data}' not in enum {schema['enum']}"
        return None
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    parser.add_argument("--interface", default=DEFAULT_INTERFACE)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}

    signals_before = fetch_signals(args.base_url, headers)
    structured_before = signals_before.get("ai_structured_submissions", 0)
    retries_before = signals_before.get("ai_schema_validation_retries", 0)

    r = requests.post(
        f"{args.base_url}/api/workflows/run-adhoc",
        json={"jcwf": build_jcwf(args.interface), "cleanup_policy": "ttl_1h"},
        headers=headers, verify=False, timeout=30,
    )
    if r.status_code not in (200, 202):
        print(f"FAIL: run-adhoc returned {r.status_code}: {r.text[:200]}")
        return 1
    run_id = r.json().get("runId") or r.json().get("run_id")
    if not run_id:
        print(f"FAIL: no runId in response: {r.text[:200]}")
        return 1
    print(f"Submitted run: {run_id}")

    state = poll_run_state(args.base_url, headers, run_id, args.timeout_seconds)
    if state != "succeeded":
        print(f"FAIL: expected 'succeeded', got '{state}'")
        return 1

    rf = requests.get(
        f"{args.base_url}/api/workflow-runs/{run_id}/files",
        headers=headers, verify=False, timeout=10,
    )
    if rf.status_code != 200:
        print(f"FAIL: files listing returned {rf.status_code}")
        return 1
    files = rf.json().get("files", [])

    # Contract: output_schema set → .output.json; no .output.txt.
    json_entry = next((f for f in files if f.get("path", "").endswith(".output.json")), None)
    txt_entry = next((f for f in files if f.get("path", "").endswith(".output.txt")), None)
    if json_entry is None:
        print(f"FAIL: no .output.json among {len(files)} run files")
        return 1
    if txt_entry is not None:
        print(f"FAIL: .output.txt present alongside .output.json — should be one or the other")
        print(f"     txt path: {txt_entry['path']}")
        return 1

    rc = requests.get(
        f"{args.base_url}/api/workflow-runs/{run_id}/files/{json_entry['path']}",
        headers=headers, verify=False, timeout=10,
    )
    if rc.status_code != 200:
        print(f"FAIL: .output.json fetch returned {rc.status_code}")
        return 1

    try:
        data = json.loads(rc.text)
    except json.JSONDecodeError as e:
        print(f"FAIL: .output.json is not valid JSON: {e}")
        print(f"     first 200 chars: {rc.text[:200]!r}")
        return 1

    err = validate_schema(data, OUTPUT_SCHEMA)
    if err:
        print(f"FAIL: .output.json does not conform to declared schema: {err}")
        print(f"     content: {data!r}")
        return 1

    if data["answer"] != 6:
        print(f"WARN: answer was {data['answer']} (expected 6 for hexagon sides)")
    else:
        print(f"OK: structured reply is correct and schema-conformant: {data}")

    signals_after = fetch_signals(args.base_url, headers)
    structured_delta = signals_after.get("ai_structured_submissions", 0) - structured_before
    retries_delta = signals_after.get("ai_schema_validation_retries", 0) - retries_before

    if structured_delta < 1:
        print(f"FAIL: ai_structured_submissions did not increment (delta={structured_delta})")
        return 1

    budget = 3  # matches output_retries above
    if retries_delta > budget:
        print(f"FAIL: ai_schema_validation_retries grew by {retries_delta}, exceeds budget {budget}")
        return 1

    print(f"OK: ai_structured_submissions incremented by {structured_delta}, "
          f"retries by {retries_delta} (budget {budget}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
