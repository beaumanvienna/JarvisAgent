#!/usr/bin/env python3
"""
JarvisAgent Workflow Test Runner

Usage:
    python test/run_tests.py --workflow make-example   # test one workflow
    python test/run_tests.py --all                     # test all in run_order
    python test/run_tests.py --list                    # list available workflows

The script assumes JarvisAgent is already running (start it in a terminal first).
It uses the REST API to clean, run, poll, and verify each workflow.
"""

import argparse
import glob
import json
import os
import re
import sys
import time
from pathlib import Path

try:
    import requests
except ImportError:
    print("ERROR: 'requests' package not found. Install with: pip install requests")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
CONFIG_PATH = SCRIPT_DIR / "test_config.json"
LOG_PATH = SCRIPT_DIR / "log.txt"

_ANSI_RE = re.compile(r'\033\[[0-9;]*m')

class TeeWriter:
    """Duplicates writes to both the original stream and a log file (ANSI-stripped)."""
    def __init__(self, stream, log_file):
        self._stream = stream
        self._log = log_file

    def write(self, text):
        self._stream.write(text)
        self._stream.flush()
        self._log.write(_ANSI_RE.sub('', text))
        self._log.flush()

    def flush(self):
        self._stream.flush()
        self._log.flush()

    def fileno(self):
        return self._stream.fileno()

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------
class C:
    RESET  = "\033[0m"
    BOLD   = "\033[1m"
    RED    = "\033[91m"
    GREEN  = "\033[92m"
    YELLOW = "\033[93m"
    CYAN   = "\033[96m"
    DIM    = "\033[2m"

def ok(msg):     print(f"  {C.GREEN}✓{C.RESET} {msg}")
def fail(msg):   print(f"  {C.RED}✗{C.RESET} {msg}")
def info(msg):   print(f"  {C.CYAN}ℹ{C.RESET} {msg}")
def warn(msg):   print(f"  {C.YELLOW}⚠{C.RESET} {msg}")
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'─'*60}\n  {msg}\n{'─'*60}{C.RESET}")

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
def load_config():
    with open(CONFIG_PATH) as f:
        return json.load(f)

# ---------------------------------------------------------------------------
# API helpers
# ---------------------------------------------------------------------------
class JarvisAPI:
    def __init__(self, base_url):
        self.base_url = base_url.rstrip("/")
        self._session = requests.Session()
        if self.base_url.startswith("https"):
            import urllib3
            urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
            self._session.verify = False

    def _url(self, path):
        return f"{self.base_url}{path}"

    def health_check(self):
        """Returns True if JA is reachable."""
        try:
            r = self._session.get(self._url("/api/status"), timeout=5)
            return r.status_code == 200
        except requests.ConnectionError:
            return False

    def get_status(self):
        r = self._session.get(self._url("/api/status"), timeout=5)
        r.raise_for_status()
        return r.json()

    def clean_workflow(self, workflow_id):
        r = self._session.delete(self._url(f"/api/workflows/{workflow_id}/clean"), timeout=60)
        return r.status_code, r.json()

    def run_workflow(self, workflow_id, context=None):
        body = {}
        if context:
            body["context"] = context
        r = self._session.post(self._url(f"/api/workflows/{workflow_id}/run"),
                               json=body, timeout=10)
        return r.status_code, r.json()

    def get_active_runs(self):
        r = self._session.get(self._url("/api/workflow-runs/active"), timeout=5)
        r.raise_for_status()
        return r.json()

    def get_last_runs(self):
        r = self._session.get(self._url("/api/workflow-runs/last"), timeout=5)
        r.raise_for_status()
        return r.json()

    def get_run_detail(self, run_id):
        r = self._session.get(self._url(f"/api/workflow-runs/{run_id}"), timeout=5)
        r.raise_for_status()
        return r.json()

    def get_registered_workflow_ids(self):
        """Returns a set of workflow IDs currently registered in JarvisAgent."""
        r = self._session.get(self._url("/api/workflows"), timeout=5)
        r.raise_for_status()
        data = r.json()
        return {w["id"] for w in data.get("workflows", [])}

# ---------------------------------------------------------------------------
# Prerequisite checks
# ---------------------------------------------------------------------------
def check_prerequisites(prereqs):
    """Returns list of missing prerequisites."""
    missing = []
    for p in prereqs:
        if p == "__polarion_mock__":
            try:
                r = requests.get("http://localhost:18080/polarion/rest/v1/health", timeout=3)
                if r.status_code >= 400:
                    missing.append("Polarion mock (http://localhost:18080)")
            except requests.ConnectionError:
                missing.append("Polarion mock (http://localhost:18080)")
        else:
            full_path = PROJECT_ROOT / p
            if not full_path.exists():
                missing.append(str(full_path))
    return missing

# ---------------------------------------------------------------------------
# Artifact verification
# ---------------------------------------------------------------------------
def verify_artifacts(expected_artifacts):
    """Returns (passed, details) list."""
    results = []
    for spec in expected_artifacts:
        pattern = str(PROJECT_ROOT / spec["glob"])
        min_bytes = spec.get("min_bytes", 1)
        min_count = spec.get("min_count", 1)

        matches = sorted(glob.glob(pattern))

        if len(matches) < min_count:
            results.append((False, f"glob '{spec['glob']}': found {len(matches)} files, expected >= {min_count}"))
            continue

        too_small = []
        for m in matches:
            size = os.path.getsize(m)
            if size < min_bytes:
                too_small.append((os.path.relpath(m, PROJECT_ROOT), size))

        if too_small:
            names = ", ".join(f"{n} ({s}B)" for n, s in too_small[:5])
            results.append((False, f"glob '{spec['glob']}': {len(too_small)} file(s) below {min_bytes}B: {names}"))
        else:
            total_size = sum(os.path.getsize(m) for m in matches)
            results.append((True, f"glob '{spec['glob']}': {len(matches)} file(s), total {total_size:,}B"))

    return results

# ---------------------------------------------------------------------------
# Poll until workflow run finishes
# ---------------------------------------------------------------------------
def poll_run(api, run_id, timeout_sec, poll_interval):
    """Polls until the run completes. Returns final run detail dict or None on timeout."""
    deadline = time.time() + timeout_sec
    last_progress = ""

    while time.time() < deadline:
        try:
            data = api.get_run_detail(run_id)
        except requests.HTTPError as e:
            if e.response is not None and e.response.status_code == 404:
                # Run not registered yet — silently retry
                time.sleep(poll_interval)
                continue
            warn(f"poll error: {e}")
            time.sleep(poll_interval)
            continue
        except Exception as e:
            warn(f"poll error: {e}")
            time.sleep(poll_interval)
            continue

        run = data.get("run", {})
        state = run.get("state", "unknown")
        tasks = run.get("tasks", [])

        # Progress summary
        succeeded = sum(1 for t in tasks if t.get("state") in ("succeeded", "skipped"))
        failed    = sum(1 for t in tasks if t.get("state") == "failed")
        total     = len(tasks)
        progress  = f"{succeeded}/{total} done"
        if failed:
            progress += f", {failed} failed"

        if progress != last_progress:
            info(f"state={state}  {progress}")
            last_progress = progress

        if state in ("succeeded", "failed", "cancelled"):
            return data

        time.sleep(poll_interval)

    return None

# ---------------------------------------------------------------------------
# Run a single workflow test
# ---------------------------------------------------------------------------
def test_workflow(api, workflow_id, wf_config, cfg):
    header(f"{workflow_id}  —  {wf_config.get('label', '')}")

    # 1. Prerequisites
    prereqs = wf_config.get("prerequisites", [])
    if prereqs:
        missing = check_prerequisites(prereqs)
        if missing:
            for m in missing:
                fail(f"prerequisite missing: {m}")
            warn("SKIPPED — prerequisites not met")
            return "skipped"

    # 2. Run
    context = wf_config.get("context", {})
    info(f"Starting workflow run" + (f" with context: {json.dumps(context)}" if context else "") + "...")
    status_code, resp = api.run_workflow(workflow_id, context if context else None)

    if status_code in (200, 202):
        run_id = resp.get("runId", "")
        ok(f"run started: {run_id}")
    else:
        fail(f"run failed ({status_code}): {resp}")
        return "failed"

    # 4. Poll
    timeout_sec = wf_config.get("timeout_sec", cfg.get("default_timeout_sec", 300))
    poll_interval = cfg.get("poll_interval_sec", 3)
    info(f"Polling (timeout {timeout_sec}s)...")

    result = poll_run(api, run_id, timeout_sec, poll_interval)

    if result is None:
        fail(f"TIMEOUT after {timeout_sec}s")
        return "timeout"

    run = result.get("run", {})
    final_state = run.get("state", "unknown")
    tasks = run.get("tasks", [])

    if final_state == "succeeded":
        ok(f"workflow completed: {final_state}")
    else:
        fail(f"workflow completed: {final_state}")
        # Show failed tasks
        failed_tasks = [t for t in tasks if t.get("state") == "failed"]
        for ft in failed_tasks[:10]:
            fail(f"  task '{ft['taskId']}': {ft.get('lastError', 'no error message')}")

    # 5. Verify artifacts
    expected = wf_config.get("expected_artifacts", [])
    if expected:
        info("Verifying artifacts...")
        artifact_results = verify_artifacts(expected)
        all_pass = True
        for passed, detail in artifact_results:
            if passed:
                ok(detail)
            else:
                fail(detail)
                all_pass = False

        if not all_pass and final_state == "succeeded":
            final_state = "artifacts_failed"

    # 6. Summary
    print()
    task_summary = {}
    for t in tasks:
        s = t.get("state", "unknown")
        task_summary[s] = task_summary.get(s, 0) + 1
    summary_parts = [f"{v} {k}" for k, v in sorted(task_summary.items())]
    info(f"Task summary: {', '.join(summary_parts)}")

    # List artifact paths for Cascade to inspect
    if expected:
        info("Artifact paths for inspection:")
        for spec in expected:
            pattern = str(PROJECT_ROOT / spec["glob"])
            matches = sorted(glob.glob(pattern))
            for m in matches[:5]:
                print(f"      {C.DIM}{m}{C.RESET}")
            if len(matches) > 5:
                print(f"      {C.DIM}... and {len(matches) - 5} more{C.RESET}")

    return final_state

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="JarvisAgent Workflow Test Runner")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--workflow", "-w", help="Test a single workflow by ID")
    group.add_argument("--all", "-a", action="store_true", help="Test all workflows in run_order")
    group.add_argument("--list", "-l", action="store_true", help="List available workflows")
    parser.add_argument("--base-url", default=None, help="JA base URL (overrides config)")
    parser.add_argument("--yes", "-y", action="store_true", help="Skip interactive prompts (auto-clean + auto-start)")
    args = parser.parse_args()

    # Tee all output to test/log.txt
    log_file = open(LOG_PATH, "w", encoding="utf-8")
    sys.stdout = TeeWriter(sys.__stdout__, log_file)

    cfg = load_config()
    base_url = args.base_url or cfg.get("base_url", "http://localhost:8080")
    api = JarvisAPI(base_url)
    workflows = cfg.get("workflows", {})
    run_order = cfg.get("run_order", list(workflows.keys()))

    # List mode
    if args.list:
        print(f"\n{C.BOLD}Available workflows:{C.RESET}\n")
        for wf_id in run_order:
            wf = workflows.get(wf_id, {})
            prereqs = wf.get("prerequisites", [])
            prereq_str = f"  {C.YELLOW}(requires: {', '.join(prereqs)}){C.RESET}" if prereqs else ""
            print(f"  {C.CYAN}{wf_id:<35}{C.RESET} {wf.get('label', '')}{prereq_str}")
        print()
        return

    # Health check
    print(f"\n{C.BOLD}JarvisAgent Test Runner{C.RESET}")
    info(f"Base URL: {base_url}")

    if not api.health_check():
        fail(f"Cannot reach JarvisAgent at {base_url}")
        fail("Please start JA in a terminal first, then re-run this script.")
        sys.exit(1)

    status = api.get_status()
    ok("Connected to JarvisAgent")
    print()
    info(f"Workflows registered:    {status.get('workflows_registered', '?')}")
    info(f"Active workflow runs:     {status.get('workflow_runs_active', '?')}")
    info(f"Session managers:         {status.get('session_managers_total', '?')}")
    info(f"Sessions with in-flight:  {status.get('session_managers_with_inflight', '?')}")
    info(f"Total queries in-flight:  {status.get('session_managers_inflight_total', '?')}")
    info(f"WebSocket clients:        {status.get('websocket_clients', '?')}")

    # Show last completed runs if any
    try:
        last_runs = api.get_last_runs()
        runs = last_runs.get("runs", [])
        if runs:
            print()
            info("Last completed runs:")
            for r in runs:
                state = r.get("state", "?")
                color = C.GREEN if state == "succeeded" else C.RED
                print(f"      {color}{r.get('workflowId', '?'):<30}{C.RESET} "
                      f"{state:<12} {r.get('taskCount', '?')} tasks  "
                      f"{C.DIM}{r.get('completedAt', '')}{C.RESET}")
    except Exception:
        pass

    # Reload workflows from disk so any JCWF edits are picked up
    info("Reloading workflows from disk...")
    try:
        r = api._session.post(api._url("/api/workflows/reload"), timeout=10)
        if r.status_code == 200:
            data = r.json()
            ok(f"Workflows reloaded ({data.get('workflowCount', '?')} workflows)")
        else:
            warn(f"Reload returned {r.status_code}: {r.text}")
    except Exception as e:
        warn(f"Reload failed: {e}")
    print()

    # Check if workflows/ folder has any .jcwf files
    workflows_dir = PROJECT_ROOT / "workflows"
    workflows_dir.mkdir(exist_ok=True)
    jcwf_files = list(workflows_dir.glob("*.jcwf"))
    if not jcwf_files:
        warn("No .jcwf files found in workflows/ folder.")
        answer = input(f"  {C.YELLOW}Copy example/workflows/make-example.jcwf into workflows/? [y/N]: {C.RESET}").strip().lower()
        if answer in ("y", "yes"):
            import shutil
            src = PROJECT_ROOT / "example" / "workflows" / "make-example.jcwf"
            dst = workflows_dir / "make-example.jcwf"
            if src.exists():
                shutil.copy2(src, dst)
                ok(f"Copied {src.name} → workflows/")
                info("Reloading workflows in JA...")
                try:
                    r = api._session.post(api._url("/api/workflows/reload"), timeout=10)
                    if r.status_code == 200:
                        ok(f"Workflows reloaded: {r.json()}")
                    else:
                        warn(f"Reload returned {r.status_code}: {r.text}")
                except Exception as e:
                    warn(f"Reload failed: {e}")
            else:
                fail(f"Source file not found: {src}")
                sys.exit(1)
        else:
            fail("No workflows to test. Add .jcwf files to workflows/ and restart JA.")
            sys.exit(1)

    # Determine which workflows to test
    if args.workflow:
        if args.workflow not in workflows:
            fail(f"Unknown workflow '{args.workflow}'. Use --list to see available workflows.")
            sys.exit(1)
        to_test = [args.workflow]
    else:
        to_test = run_order

    # Fetch the set of workflow IDs actually registered in JarvisAgent.
    try:
        registered_ids = api.get_registered_workflow_ids()
    except Exception as e:
        warn(f"Could not fetch registered workflows: {e}")
        registered_ids = None  # will skip the pre-check

    # Prompt to clean all workflows before testing
    print()
    if args.yes:
        answer = "y"
    else:
        answer = input(f"  {C.YELLOW}Clean all workflow outputs before testing? [y/N]: {C.RESET}").strip().lower()
    if answer in ("y", "yes"):
        info("Cleaning all workflows...")
        for wf_id in to_test:
            if registered_ids is not None and wf_id not in registered_ids:
                warn(f"{wf_id}: not registered, skipped clean")
                continue
            try:
                status_code, resp = api.clean_workflow(wf_id)
                if status_code == 200:
                    ok(f"cleaned {wf_id}")
                elif status_code == 409:
                    warn(f"{wf_id}: still running, skipped clean")
                else:
                    warn(f"{wf_id}: clean returned {status_code}")
            except Exception as e:
                fail(f"{wf_id}: clean error — {e}")
                info("Waiting for JA to recover...")
                time.sleep(3)
                if not api.health_check():
                    fail("JA is not responding. Please restart JA and re-run.")
                    sys.exit(1)
            time.sleep(0.3)
    else:
        info("Skipping clean — using existing outputs.")

    # List workflows to be tested and prompt before starting
    print()
    info(f"Workflows to test ({len(to_test)}):\n")
    for i, wf_id in enumerate(to_test, 1):
        wf = workflows.get(wf_id, {})
        print(f"      {C.CYAN}{i:>2}. {wf_id:<30}{C.RESET} {wf.get('label', '')}")
    print()
    if not args.yes:
        input(f"  {C.YELLOW}Press Enter to start testing...{C.RESET}")

    # Run tests
    results = {}
    for wf_id in to_test:
        wf_config = workflows.get(wf_id)
        if not wf_config:
            warn(f"No config for '{wf_id}', skipping")
            continue

        if registered_ids is not None and wf_id not in registered_ids:
            header(f"{wf_id}  —  {wf_config.get('label', '')}")
            warn(f"workflow '{wf_id}' is not registered in JarvisAgent — SKIPPED")
            results[wf_id] = "skipped"
            continue

        outcome = test_workflow(api, wf_id, wf_config, cfg)
        results[wf_id] = outcome

    # Final report
    header("TEST RESULTS")
    passed = 0
    failed = 0
    skipped = 0
    for wf_id, outcome in results.items():
        if outcome == "succeeded":
            ok(f"{wf_id}: PASSED")
            passed += 1
        elif outcome == "skipped":
            warn(f"{wf_id}: SKIPPED")
            skipped += 1
        else:
            fail(f"{wf_id}: FAILED ({outcome})")
            failed += 1

    print(f"\n  {C.BOLD}Total: {passed} passed, {failed} failed, {skipped} skipped{C.RESET}\n")
    sys.exit(1 if failed > 0 else 0)

if __name__ == "__main__":
    main()
