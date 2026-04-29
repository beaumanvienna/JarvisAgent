"""
Shared helpers for the §14 TUI stress tests.

These tests exercise the TUI ncurses renderer's UTF-8 byte safety by driving
heavy or malformed reply text through the full j9t pipeline (TestInterface
short-circuit → log macros → ncurses TUI + log/log.txt + dashboard WS).
The point is regression armor for the 2026-04-27 truncation bug class.

Importantly:
  - We use the existing TestInterface short-circuit (aiRequestPool.cpp:1119) —
    it reads `api->m_Url` as a fixture path and synthesizes the reply, so
    the bytes flow into the renderer without any real network call.
  - The three jarvisCpp JCWFs reference CNTX files via relative paths
    (`../../../application/...`) that resolve correctly when the workflow
    runs from `workflows/<name>/`.  Adhoc submission relocates the working
    directory to `_adhoc/<run>/...`, breaking those relative paths.  This
    helper rewrites every relative cntx_files entry to its absolute
    equivalent (resolved relative to the original workflow location).
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

import requests


REPO_ROOT = Path("/home/beaumanvienna/dev/jarvisAgent")


def load_jarvisCpp_workflow(workflow_name: str, *,
                            interface_override: str,
                            id_suffix: str) -> dict:
    """Load `workflows/<workflow_name>/global.json` + `<workflow_name>.json`,
    merge them, override every ai_call task's `params.provider`, rewrite
    cntx_files relative paths to absolute, and return a JCWF dict suitable
    for /api/workflows/run-adhoc.

    `id_suffix` is appended to the workflow id so concurrent submissions
    don't collide on disk."""
    wf_dir = REPO_ROOT / "workflows" / workflow_name
    if not wf_dir.is_dir():
        raise FileNotFoundError(f"workflow folder not found: {wf_dir}")

    # Merge global.json + tasks file.  global.json holds workflow-level
    # metadata; <name>.json holds the tasks dict.
    g = json.loads((wf_dir / "global.json").read_text(encoding="utf-8"))
    t = json.loads((wf_dir / f"{workflow_name}.json").read_text(encoding="utf-8"))
    jcwf = dict(g)
    jcwf["tasks"] = t.get("tasks", {})

    # Unique workflow id per run so 3 concurrent submissions don't collide.
    jcwf["id"] = f"{jcwf.get('id', workflow_name)}_{id_suffix}"

    # For every ai_call task: set provider override + absolutize cntx_files.
    for task_id, task in jcwf["tasks"].items():
        if not isinstance(task, dict) or task.get("type") != "ai_call":
            continue

        # Provider override drives the dispatch to TestInterface.  AiCall-
        # TaskExecutor reads params.provider per task.
        params = task.setdefault("params", {})
        if not isinstance(params, dict):
            params = {}
            task["params"] = params
        params["provider"] = interface_override

        # Resolve cntx_files relative paths.  Original task working dir is
        #   <wf_dir>/<task['working_directory']>
        # which for these JCWFs lands at queue/<workflow>/<task_dir> under
        # the repo root.  CNTX paths are relative to that working dir.
        task_wd_rel = task.get("working_directory", "")
        if task_wd_rel:
            # working_directory is relative to the workflow file's dir.
            orig_task_wd = (wf_dir / task_wd_rel).resolve()
        else:
            orig_task_wd = wf_dir

        qb = task.get("queue_binding", {})
        cntx = qb.get("cntx_files", [])
        new_cntx = []
        for entry in cntx:
            if isinstance(entry, str):
                p = Path(entry)
                if not p.is_absolute():
                    new_cntx.append(str((orig_task_wd / entry).resolve()))
                else:
                    new_cntx.append(entry)
            else:
                # Inline-content cntx file ({"path": ..., "content": ...}) — keep as-is.
                new_cntx.append(entry)
        if new_cntx:
            qb["cntx_files"] = new_cntx
            task["queue_binding"] = qb

    return jcwf


def submit_adhoc(base_url: str, headers: dict, jcwf: dict,
                 cleanup_policy: str = "on_completion",
                 timeout_s: float = 60.0) -> str | None:
    r = requests.post(f"{base_url}/api/workflows/run-adhoc",
                      json={"jcwf": jcwf, "cleanup_policy": cleanup_policy},
                      headers=headers, verify=False, timeout=timeout_s)
    if r.status_code not in (200, 202):
        print(f"FAIL: run-adhoc returned {r.status_code}: {r.text[:300]}")
        return None
    payload = r.json()
    return payload.get("runId") or payload.get("run_id")


def poll_run_state(base_url: str, headers: dict, run_id: str,
                   timeout_s: float = 300.0) -> str | None:
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
        time.sleep(0.5)
    return None


def j9t_alive(base_url: str, headers: dict) -> bool:
    """j9t process is the renderer host — if it crashed (segfault / abort
    from ncurses), no socket replies."""
    try:
        r = requests.get(f"{base_url}/api/status",
                         headers=headers, verify=False, timeout=5)
        return r.status_code in (200, 401)  # 401 OK — process up, just unauth'd
    except requests.RequestException:
        return False


def log_file_path() -> Path:
    return REPO_ROOT / "log" / "log.txt"


def log_file_size() -> int:
    p = log_file_path()
    return p.stat().st_size if p.exists() else 0


def log_tail_is_valid_utf8(start_offset: int = 0) -> tuple[bool, str | None]:
    """Read log/log.txt from `start_offset` to EOF and try to decode as UTF-8.
    Returns (ok, error_message).  The TUI's invariant: bytes in the log file
    must always be well-formed UTF-8 even when source content was malformed.
    A False return means the sanitization layer let bad bytes through."""
    p = log_file_path()
    if not p.exists():
        return True, None
    with open(p, "rb") as f:
        f.seek(start_offset)
        chunk = f.read()
    try:
        chunk.decode("utf-8")
        return True, None
    except UnicodeDecodeError as e:
        # Slice some context for the error.
        bad = chunk[max(0, e.start - 16):e.start + 16]
        return False, f"{e!s} — context bytes: {bad!r}"


def provision_test_interface(base_url: str, headers: dict, *,
                              name: str, fixture_path: Path) -> bool:
    """Create a TestInterface pointing at `fixture_path`.  TestInterface short-
    circuits the dispatcher and synthesizes a reply from the fixture content."""
    body = {
        "name": name,
        "description": f"§14 stress test fixture — {fixture_path.name}",
        "url": str(fixture_path.resolve()),
        "model": "stress-stub",
        "api_type": "Test",
        "key_name": "",
    }
    r = requests.post(f"{base_url}/api/settings/ai-interfaces",
                      json=body, headers=headers, verify=False, timeout=10)
    if r.status_code in (200, 201):
        return True
    if r.status_code == 409:
        return True
    print(f"FAIL: provision_test_interface({name}) returned {r.status_code}: {r.text[:200]}")
    return False


def cleanup_test_interface(base_url: str, headers: dict, name: str) -> None:
    try:
        requests.delete(f"{base_url}/api/settings/ai-interfaces/{name}",
                        headers=headers, verify=False, timeout=10)
    except Exception:
        pass
