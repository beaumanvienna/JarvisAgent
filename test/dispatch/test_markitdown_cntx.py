#!/usr/bin/env python3
"""
Markitdown auto-conversion contract test.

When a CNTX file reference points at an office-format file (.pdf, .docx,
.xlsx, etc.), the executor runs `markitdown` synchronously and substitutes
the resulting markdown for the file's text.  This test proves that:

  - The executor recognises the .pdf extension and invokes markitdown.
  - The resulting markdown is materialized on disk as a CNTX_*.md artefact
    inside the run folder (`.pdf` → `.md` rewrite per §3 Phase 6).
  - The dispatched envelope completes successfully (through the Test
    interface so we don't send ~8 MB of extracted text to a live provider).

Requires:
  - `markitdown` CLI on PATH (pip install markitdown).
  - An 8 MB sample PDF at workflows/in.pdf (pre-existing).

Usage:
    export J9T_TOKEN=mcp_...
    python3 test/dispatch/test_markitdown_cntx.py
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

TEST_INTERFACE_NAME = "markitdown_test_dispatch"
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_FIXTURE = os.path.join(REPO_ROOT, "test", "dispatch", "fixtures", "api1", "golden_success.json")
PDF_PROJECT_RELATIVE_DEFAULT = "workflows/in.pdf"

# Working_directory text — joined against the adhoc workflow base
# (<projectRoot>/_adhoc/<userSlug>/<folderName>/workflows/<workflowId>/, i.e.
# the extracted JCWF folder) and normalised, so it resolves to
# <projectRoot>/_adhoc/<userSlug>/<folderName>/queue/adhoc_markitdown_dispatch/01_ingest.
TASK_WORKING_DIRECTORY = "../../queue/adhoc_markitdown_dispatch/01_ingest"

# Hops from the resolved working_directory back to project root.  Six
# segments deep (_adhoc, <userSlug>, <folderName>, queue, <workflowId>, <task>)
# ⇒ six "../".  Used to build the cntx_files path reference; the parser
# rejects absolute paths (IsAcceptedRelativePath in workflowJsonParserDetails.h),
# so we transmit a relative path that the executor resolves back to the
# on-disk PDF.
WORKING_DIR_DEPTH = 6


def build_jcwf(interface_name: str, pdf_project_relative: str) -> dict:
    cntx_path = ("../" * WORKING_DIR_DEPTH) + pdf_project_relative
    return {
        "version": "1.0",
        "id": "adhoc_markitdown_dispatch",
        "label": "Contract: markitdown CNTX auto-conversion",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "ingest": {
                "id": "ingest",
                "type": "ai_call",
                "label": "Ingest a PDF via markitdown",
                "mode": "single",
                "working_directory": TASK_WORKING_DIRECTORY,
                "params": {"provider": interface_name},
                "queue_binding": {
                    "stng_files": [{"path": "STNG_x.txt",
                                    "content": "Summarise in one line."}],
                    "task_files": [{"path": "TASK_x.txt",
                                    "content": "Ingest the PDF reference."}],
                    # Path reference (bare string) so the executor sees the
                    # .pdf extension and auto-runs markitdown.  Relative form
                    # required: the parser rejects absolute paths, and
                    # ConfineUnderProjectRoot in the executor then accepts the
                    # canonical resolution because it lands inside the project
                    # tree.
                    "cntx_files": [cntx_path],
                    "prob_files": [{"path": "PROB_x.txt",
                                    "content": "Describe the document in one sentence."}],
                },
            }
        },
    }


def create_test_interface(base_url: str, headers: dict, fixture_path: str) -> bool:
    body = {
        "name": TEST_INTERFACE_NAME,
        "description": "Markitdown test — MockTransport backend, network-free",
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
    return r.status_code in (200, 201)


def delete_test_interface(base_url: str, headers: dict) -> None:
    try:
        requests.delete(
            f"{base_url}/api/settings/ai-interfaces/{TEST_INTERFACE_NAME}",
            headers=headers, verify=False, timeout=10,
        )
    except Exception:
        pass


def poll_run_state(base_url: str, headers: dict, run_id: str, timeout_s: int = 300) -> str | None:
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    parser.add_argument("--pdf-relative-path", default=PDF_PROJECT_RELATIVE_DEFAULT,
                        help="PDF location relative to the project root "
                             "(server-side resolution).")
    parser.add_argument("--timeout-seconds", type=int, default=300)
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1
    abs_pdf_path = os.path.join(REPO_ROOT, args.pdf_relative_path)
    if not os.path.isfile(abs_pdf_path):
        print(f"FAIL: PDF fixture not found at {abs_pdf_path}")
        return 1
    if not os.path.isfile(FIXTURE_FIXTURE):
        print(f"FAIL: hermetic reply fixture not found at {FIXTURE_FIXTURE}")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}

    if not create_test_interface(args.base_url, headers, FIXTURE_FIXTURE):
        print("FAIL: could not create Test interface")
        return 1

    try:
        print(f"Submitting with CNTX → {args.pdf_relative_path} "
              f"({os.path.getsize(abs_pdf_path):,} bytes)")
        r = requests.post(
            f"{args.base_url}/api/workflows/run-adhoc",
            json={"jcwf": build_jcwf(TEST_INTERFACE_NAME, args.pdf_relative_path),
                  "cleanup_policy": "ttl_1h"},
            headers=headers, verify=False, timeout=60,
        )
        if r.status_code not in (200, 202):
            print(f"FAIL: run-adhoc returned {r.status_code}: {r.text[:300]}")
            return 1
        run_id = r.json().get("runId") or r.json().get("run_id")
        if not run_id:
            print(f"FAIL: no runId in response: {r.text[:200]}")
            return 1
        print(f"Submitted run: {run_id} (markitdown conversion can take tens of seconds…)")

        state = poll_run_state(args.base_url, headers, run_id, args.timeout_seconds)
        if state != "succeeded":
            print(f"FAIL: expected 'succeeded', got '{state}' after {args.timeout_seconds}s")
            # Pull run log for diagnostic
            rs = requests.get(
                f"{args.base_url}/api/workflow-runs/{run_id}",
                headers=headers, verify=False, timeout=10,
            )
            if rs.status_code == 200:
                run = rs.json().get("run") or rs.json()
                tasks = run.get("tasks", [])
                for t in tasks:
                    err = t.get("error") or t.get("last_error_message")
                    if err:
                        print(f"     task {t.get('id')} error: {err}")
            return 1

        rf = requests.get(
            f"{args.base_url}/api/workflow-runs/{run_id}/files",
            headers=headers, verify=False, timeout=10,
        )
        if rf.status_code != 200:
            print(f"FAIL: files listing returned {rf.status_code}")
            return 1
        files = rf.json().get("files", [])

        # Success invariant #1: the converted CNTX landed on disk as a .md file.
        md_entry = next((f for f in files
                         if f.get("path", "").endswith(".md")
                         and "CNTX" in f.get("path", "")), None)
        if md_entry is None:
            print(f"FAIL: no CNTX_*.md materialised in run folder "
                  f"({len(files)} files total). Markitdown conversion was not "
                  f"triggered or the output was not recorded.")
            for f in files[:20]:
                print(f"     {f.get('path')}")
            return 1

        # Success invariant #2: the converted markdown is non-trivial.
        size = md_entry.get("size_bytes", md_entry.get("size", 0))
        if size < 200:
            print(f"FAIL: CNTX_*.md is suspiciously small ({size} bytes). "
                  f"Markitdown may have produced an empty or stub output.")
            return 1

        # Success invariant #3: .output.txt landed (dispatch completed).
        output_entry = next((f for f in files if f.get("path", "").endswith(".output.txt")), None)
        if output_entry is None:
            print("FAIL: no .output.txt — dispatch did not complete end-to-end")
            return 1

        print(f"OK: markitdown converted {os.path.basename(args.pdf_relative_path)} → "
              f"{md_entry['path']} ({size:,} bytes), dispatch reached succeeded.")
        return 0

    finally:
        delete_test_interface(args.base_url, headers)


if __name__ == "__main__":
    sys.exit(main())
