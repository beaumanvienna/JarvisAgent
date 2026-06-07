#!/usr/bin/env python3
"""
Live-AI contract test: a CNTX body larger than the interface's
`max_context_tokens` triggers chunked fan-out and reduce-pass consolidation.

What's exercised (Phase 6):
  - `ChunkPlanner::Plan` splits the oversized CNTX at section boundaries.
  - `AiCallTaskExecutor` emits one envelope per chunk + one reduce envelope.
  - `AiRequestPool` increments the `ai_chunked_dispatches` counter.
  - The final reduced reply lands as `<prob>.output.txt` (concat fallback is
    only triggered when a chunk errors).

Gated `--with-ai` — costs real tokens (N chunks + 1 reduce = N+1 calls).

Uses the hermetic Test interface so NO network call is made.  The Test
backend's `model="hermetic-stub"` doesn't match the fallback table, so
`max_context_tokens` resolves to the unknown-model fallback of 50 000.  We
send ~80 000 tokens of markdown CNTX to guarantee chunking fires.

The Test path writes the fixture content back as each chunk's reply; the
reduce envelope receives N copies of the fixture and also gets the fixture
back.  We don't assert on reply *content* (all-fixture input is nonsensical
for reduce); we assert on:
  - dispatch invariants (ai_chunked_dispatches increments, run succeeds)
  - file-contract invariants (final .output.txt present)

Usage:
    export J9T_TOKEN=mcp_...
    python3 test/dispatch/test_chunking_fanout.py
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _provisioning  # noqa: E402

TEST_INTERFACE_NAME = "chunking_test_dispatch"
FIXTURE_PATH_DEFAULT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "fixtures", "api1", "golden_success.json")
)

# Explicit override — the REST create handler accepts `max_context_tokens`
# on the request body and uses it instead of the model-name fallback.
# A small value means a tiny CNTX forces chunking, keeping the test fast.
MAX_CONTEXT_TOKENS = 2000

# Send ~3× the budget so the planner produces at least 2–3 chunks after
# accounting for STNG/TASK/PROB overhead and 20% response reservation.
TARGET_TOKENS = 6000


def build_cntx_body() -> str:
    """Large markdown with section boundaries so ChunkPlanner splits cleanly."""
    paragraph = (
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "
        "Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
        "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris "
        "nisi ut aliquip ex ea commodo consequat. "
    )
    sections = []
    target_chars = TARGET_TOKENS * 4
    section_index = 0
    total = 0
    while total < target_chars:
        section_index += 1
        header = f"# Section {section_index}\n\n"
        body = paragraph * 200  # ~22 k chars per section
        chunk = header + body + "\n\n"
        sections.append(chunk)
        total += len(chunk)
    return "".join(sections)


def build_jcwf(interface_name: str, cntx: str) -> dict:
    return {
        "version": "1.0",
        "id": "adhoc_chunking_dispatch",
        "label": "Contract: chunked fan-out + reduce pass",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "bigctx": {
                "id": "bigctx",
                "type": "ai_call",
                "label": "Oversized CNTX",
                "mode": "single",
                "working_directory": "../../queue/adhoc_chunking_dispatch/01_bigctx",
                "params": {"provider": interface_name},
                "queue_binding": {
                    "stng_files": [{"path": "STNG_x.txt",
                                    "content": "Summarise the provided context in one sentence."}],
                    "task_files": [{"path": "TASK_x.txt",
                                    "content": "Chunking test — ignore content quality."}],
                    "cntx_files": [{"path": "CNTX_x.txt", "content": cntx}],
                    "prob_files": [{"path": "PROB_x.txt",
                                    "content": "What is in the context?"}],
                },
            }
        },
    }


def create_test_interface(base_url: str, headers: dict, fixture_path: str) -> bool:
    body = {
        "name": TEST_INTERFACE_NAME,
        "description": "Chunking test — MockTransport interface with deliberately small window",
        "url": "https://localhost/_mock_/never_called",
        "model": "mock-stub",
        "api_type": "API1",
        "key_name": "",
        "max_context_tokens": MAX_CONTEXT_TOKENS,
        "is_mock": True,
        "fixture_path": fixture_path,
    }
    r = _provisioning.create_interface(base_url, headers, body)
    return r.status_code in (200, 201)


def delete_test_interface(base_url: str, headers: dict) -> None:
    try:
        _provisioning.delete_interface(base_url, headers, TEST_INTERFACE_NAME)
    except Exception:
        pass


def poll_run_state(base_url: str, headers: dict, run_id: str, timeout_s: int = 120) -> str | None:
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
        cntx = build_cntx_body()
        print(f"Built CNTX: {len(cntx):,} chars (~{len(cntx) // 4:,} tokens)")

        signals_before = fetch_signals(args.base_url, headers)
        chunked_before = signals_before.get("ai_chunked_dispatches", 0)

        r = requests.post(
            f"{args.base_url}/api/workflows/run-adhoc",
            json={"jcwf": build_jcwf(TEST_INTERFACE_NAME, cntx), "cleanup_policy": "ttl_1h"},
            headers=headers, verify=False, timeout=60,
        )
        if r.status_code not in (200, 202):
            print(f"FAIL: run-adhoc returned {r.status_code}: {r.text[:200]}")
            return 1
        run_id = r.json().get("runId") or r.json().get("run_id")
        if not run_id:
            print(f"FAIL: no runId in response: {r.text[:200]}")
            return 1
        print(f"Submitted run: {run_id}")

        state = poll_run_state(args.base_url, headers, run_id, timeout_s=120)
        if state != "succeeded":
            print(f"FAIL: expected 'succeeded', got '{state}'")
            return 1

        signals_after = fetch_signals(args.base_url, headers)
        chunked_delta = signals_after.get("ai_chunked_dispatches", 0) - chunked_before

        if chunked_delta < 1:
            print(f"FAIL: ai_chunked_dispatches did not increment "
                  f"(delta={chunked_delta}, body={len(cntx):,} chars)")
            print("      Chunking did not fire — either the fallback window is larger "
                  "than this test assumes, or the chunker is not invoked for this path.")
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

        # Per-chunk transcript/output artefacts stay on disk (Phase 6 contract).
        chunk_artefacts = [f for f in files
                           if ".output.chunk" in f.get("path", "")
                           or "_chunk" in f.get("path", "")]

        print(f"OK: chunked fan-out fired (ai_chunked_dispatches +{chunked_delta}); "
              f"final .output.txt present ({output_entry.get('size_bytes', 0)} bytes); "
              f"{len(chunk_artefacts)} chunk artefact(s) on disk.")
        return 0

    finally:
        delete_test_interface(args.base_url, headers)


if __name__ == "__main__":
    sys.exit(main())
