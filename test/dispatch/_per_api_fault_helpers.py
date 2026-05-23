"""
Shared driver helpers for `test_api{1..6}_mock_errors.py`.

Each per-API driver supplies a list of cases (per-fixture metadata) and the
InterfaceType string ("API1".."API6"); this module owns provisioning the mock
interface, building the adhoc JCWF, polling for terminal state, fetching the
PROV sidecar + .output.txt, scanning the log slice, and cleanup.  Per-API
drivers stay thin so adding API7 later is a copy-rename.

Per-API drivers assert on the body discriminator in `expected_log_substring`
because the parsed `m_ProviderErrorCode` + `m_ProviderErrorType` +
`CategoryToString(m_Category)` flow into the `OnRequestFailed` ERROR line.
Discriminators per InterfaceType:

  api1 + api2 + api6 → OpenAI envelope types: `insufficient_quota`,
                        `rate_limit_error`, `authentication_error`, `server_error`.
  api3              → Gemini `details[*].reason`: `USER_PROJECT_QUOTA_EXCEEDED`
                        (billing) vs `RATE_LIMIT_EXCEEDED` (throttle), plus
                        status enum (`UNAUTHENTICATED`, `UNAVAILABLE`) for the rest.
  api4              → Anthropic nested `error.type`: `credit_balance_too_low`,
                        `rate_limit_error`, `authentication_error`, `overloaded_error`.
  api5              → AWS exception short names from `__type` (prefix-stripped):
                        `ServiceQuotaExceededException`, `ThrottlingException`,
                        `AccessDeniedException`, `ModelStreamErrorException`.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib3
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

try:
    import requests
except ImportError:
    print("ERROR: requests package missing (pip install requests)")
    sys.exit(1)

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
LOG_FILE = REPO_ROOT / "log" / "log.txt"


@dataclass
class FaultCase:
    name: str
    fixture: str
    expected_run_state: str            # "succeeded" | "failed"
    expected_log_substring: Optional[str]
    expected_http_status_in_meta: Optional[int]


@dataclass
class ApiTestPlan:
    """All inputs needed to run a per-API fault suite."""
    api_type: str                  # "API1".."API6"
    fixture_dir: Path
    cases: list
    interface_prefix: str          # e.g. "mock_api1_fault_"


# ---------------------------------------------------------------------------
# REST + workflow helpers
# ---------------------------------------------------------------------------

def create_mock_interface(base_url, headers, name, api_type, fixture_path):
    """Provision a mock-flagged AI interface.  rate_limit.max_retries_429=0 +
    max_retries_transient=0 so error fixtures fail fast rather than burning
    retry-backoff time per case (the recent fix to treat 0 as a real 0)."""
    body = {
        "name": name,
        "description": f"Sitting 3 fault test — {fixture_path.name}",
        "url": "https://localhost/_mock_/never_called",
        "model": "mock-stub",
        "api_type": api_type,
        "key_name": "",
        "is_mock": True,
        "fixture_path": str(fixture_path),
        "rate_limit": {
            "max_retries_429": 0,
            "max_retries_transient": 0,
        },
    }
    r = requests.post(f"{base_url}/api/settings/ai-interfaces",
                      json=body, headers=headers, verify=False, timeout=10)
    if r.status_code in (200, 201, 409):
        return True
    print(f"FAIL [{name}]: create_mock_interface returned {r.status_code}: {r.text[:200]}")
    return False


def delete_mock_interface(base_url, headers, name):
    try:
        requests.delete(f"{base_url}/api/settings/ai-interfaces/{name}",
                        headers=headers, verify=False, timeout=10)
    except Exception:
        pass


def build_jcwf(interface_name, suffix, api_label):
    wf_id = f"adhoc_{api_label}_fault_{suffix}"
    return {
        "version": "1.0",
        "id": wf_id,
        "label": f"{api_label} fault test ({suffix})",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "echo": {
                "id": "echo",
                "type": "ai_call",
                "label": f"{api_label} fault dispatch",
                "mode": "single",
                "working_directory": f"../../queue/{wf_id}/01_echo",
                "params": {"provider": interface_name},
                "queue_binding": {
                    "stng_files": [{"path": "STNG_x.txt", "content": "Fault test."}],
                    "task_files": [{"path": "TASK_x.txt", "content": "MockTransport replies from a canned fixture."}],
                    "cntx_files": [{"path": "CNTX_x.txt", "content": "no-op context"}],
                    "prob_files": [{"path": "PROB_x.txt", "content": "no-op prob"}],
                },
            }
        },
    }


def poll_run_state(base_url, headers, run_id, timeout_s=30):
    start = time.time()
    while time.time() - start < timeout_s:
        rs = requests.get(f"{base_url}/api/workflow-runs/{run_id}",
                          headers=headers, verify=False, timeout=10)
        if rs.status_code == 200:
            body = rs.json()
            run = body.get("run") if isinstance(body.get("run"), dict) else body
            state = run.get("state")
            if state in ("succeeded", "failed", "cancelled"):
                return state
        time.sleep(0.3)
    return None


def fetch_prov_sidecar(base_url, headers, run_id):
    rf = requests.get(f"{base_url}/api/workflow-runs/{run_id}/files",
                      headers=headers, verify=False, timeout=10)
    if rf.status_code != 200:
        return None, None
    files = rf.json().get("files", [])
    prov_entry = next((f for f in files
                       if f.get("path", "").startswith("PROV_")
                       or "/PROV_" in f.get("path", "")), None)
    if prov_entry is None:
        return None, None
    rp = requests.get(f"{base_url}/api/workflow-runs/{run_id}/files/{prov_entry['path']}",
                      headers=headers, verify=False, timeout=10)
    if rp.status_code != 200:
        return None, None
    text = rp.text
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        parsed = None
    return text, parsed


def fetch_output_text(base_url, headers, run_id):
    rf = requests.get(f"{base_url}/api/workflow-runs/{run_id}/files",
                      headers=headers, verify=False, timeout=10)
    if rf.status_code != 200:
        return None
    files = rf.json().get("files", [])
    output_entry = next((f for f in files if f.get("path", "").endswith(".output.txt")), None)
    if output_entry is None:
        return None
    rc = requests.get(f"{base_url}/api/workflow-runs/{run_id}/files/{output_entry['path']}",
                      headers=headers, verify=False, timeout=10)
    if rc.status_code != 200:
        return None
    body = rc.content
    if body.startswith(b"# Model:"):
        nl = body.find(b"\n")
        if nl != -1:
            body = body[nl + 1:]
    return body


# ---------------------------------------------------------------------------
# Log capture
# ---------------------------------------------------------------------------

def log_size():
    return LOG_FILE.stat().st_size if LOG_FILE.exists() else 0


def read_log_slice(start_offset):
    if not LOG_FILE.exists():
        return b""
    with open(LOG_FILE, "rb") as f:
        f.seek(start_offset)
        return f.read()


# ---------------------------------------------------------------------------
# Per-case runner
# ---------------------------------------------------------------------------

def run_one_case(plan, base_url, headers, case, suffix, api_label):
    """Returns (ok: bool, message: str).  Self-contained per case so per-test
    failures don't cascade."""
    name = case["name"]
    interface_name = f"{plan.interface_prefix}{name}_{suffix}"
    fixture_path = plan.fixture_dir / case["fixture"]
    if not fixture_path.is_file():
        return False, f"[{name}] fixture not found: {fixture_path}"

    # Internal consistency: when expected_http_status_in_meta is set the
    # paired .meta.json must exist and carry that status.
    expected_status = case["expected_http_status_in_meta"]
    meta_path = Path(str(fixture_path) + ".meta.json")
    if expected_status is not None:
        if not meta_path.is_file():
            return False, f"[{name}] expected .meta.json missing: {meta_path}"
        meta = json.loads(meta_path.read_text())
        if meta.get("http_status") != expected_status:
            return False, (f"[{name}] meta.json http_status {meta.get('http_status')} "
                           f"!= expected {expected_status}")

    log_anchor = log_size()
    if not create_mock_interface(base_url, headers, interface_name, plan.api_type, fixture_path):
        return False, f"[{name}] failed to provision mock interface"
    try:
        r = requests.post(f"{base_url}/api/workflows/run-adhoc",
                          json={"jcwf": build_jcwf(interface_name, suffix, api_label),
                                "cleanup_policy": "ttl_1h"},
                          headers=headers, verify=False, timeout=30)
        if r.status_code not in (200, 202):
            return False, f"[{name}] run-adhoc returned {r.status_code}: {r.text[:200]}"
        payload = r.json()
        run_id = payload.get("runId") or payload.get("run_id")
        if not run_id:
            return False, f"[{name}] no runId in response"

        terminal = poll_run_state(base_url, headers, run_id, timeout_s=30)
        if terminal != case["expected_run_state"]:
            log_slice = read_log_slice(log_anchor)
            return False, (f"[{name}] expected run state '{case['expected_run_state']}' "
                           f"but got '{terminal}'; runId={run_id}; "
                           f"log tail: {log_slice[-500:]!r}")

        prov_text, _ = fetch_prov_sidecar(base_url, headers, run_id)
        if prov_text is None:
            return False, f"[{name}] no PROV sidecar found for runId={run_id}"
        if '"mocked": true' not in prov_text and '"mocked":true' not in prov_text:
            return False, f"[{name}] PROV missing 'mocked: true'; content: {prov_text[:400]!r}"
        if interface_name not in prov_text:
            return False, f"[{name}] PROV does not reference interface '{interface_name}'"

        log_slice = read_log_slice(log_anchor)
        try:
            log_slice.decode("utf-8", "strict")
        except UnicodeDecodeError as e:
            return False, f"[{name}] log/log.txt contains invalid UTF-8 (sanitization regression): {e!s}"

        if case["expected_run_state"] == "failed":
            run_id_bytes = run_id.encode("ascii")
            if run_id_bytes not in log_slice:
                return False, (f"[{name}] failure log missing runId substring '{run_id}'; "
                               f"log tail: {log_slice[-800:]!r}")
            error_line_with_runid = False
            for line in log_slice.split(b"\n"):
                if b"[error]" in line or b"[critical]" in line:
                    if run_id_bytes in line:
                        error_line_with_runid = True
                        break
            if not error_line_with_runid:
                return False, (f"[{name}] no [error]/[critical] log line carrying runId={run_id}; "
                               f"log tail: {log_slice[-800:]!r}")
        if case["expected_log_substring"] is not None:
            snippet_bytes = case["expected_log_substring"].encode("utf-8")
            if snippet_bytes not in log_slice:
                return False, (f"[{name}] expected log snippet '{case['expected_log_substring']}' "
                               f"missing; log tail: {log_slice[-800:]!r}")

        if case["expected_run_state"] == "succeeded":
            output_bytes = fetch_output_text(base_url, headers, run_id)
            if output_bytes is None:
                return False, f"[{name}] no .output.txt for succeeded run"
            try:
                output_bytes.decode("utf-8", "strict")
            except UnicodeDecodeError as e:
                return False, f"[{name}] .output.txt is not valid UTF-8: {e!s}"

        return True, f"[{name}] OK (runId={run_id}, terminal={terminal})"

    finally:
        delete_mock_interface(base_url, headers, interface_name)


def parse_args_and_run(plan, api_label):
    """Stand-alone main() body for a per-API driver.  Returns the exit code."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="https://localhost:8443")
    parser.add_argument("--token", default=os.environ.get("J9T_TOKEN"))
    parser.add_argument("--only", default=None,
                        help="Run a single case by name (e.g. error_billing)")
    args = parser.parse_args()

    if not args.token:
        print("FAIL: no MCP token provided (use --token or J9T_TOKEN env var)")
        return 1

    headers = {"Authorization": f"Bearer {args.token}"}
    suffix = str(int(time.time()))

    cases = plan.cases
    if args.only:
        cases = [c for c in plan.cases if c["name"] == args.only]
        if not cases:
            print(f"FAIL: unknown case '{args.only}'")
            return 1

    results = []
    for case in cases:
        ok, msg = run_one_case(plan, args.base_url, headers, case, suffix, api_label)
        print(msg if ok else f"FAIL {msg}")
        results.append(ok)

    total = len(results)
    passed = sum(1 for x in results if x)
    print(f"\n== test_{api_label}_mock_errors: {passed}/{total} pass ==")
    return 0 if passed == total else 1
