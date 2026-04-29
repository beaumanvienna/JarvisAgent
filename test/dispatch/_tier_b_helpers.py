"""
Shared helpers for §14 Tier B Phase B Python tests.

Each Phase B test exercises CurlMultiDispatcher via a real curl round-trip
to the localhost mock endpoint (POST /api/debug/mock-ai-response).  The
helpers in this module abstract the boilerplate: provision a stub provider
+ AI interface pointing at the mock URL, submit an adhoc workflow, poll
for completion, read back debug snapshots.

Tests import this module and call the helpers; they do NOT subclass or
share state.  Each test cleans up its own resources in `finally`.

NOT a test runner — `import _tier_b_helpers` from a sibling test_*.py.
"""

from __future__ import annotations

import time
import urllib.parse

import requests


def mock_endpoint_url(base_url: str, **params) -> str:
    """Compose `https://localhost:8443/api/debug/mock-ai-response?...` with
    the given query params.  All non-None params are URL-encoded."""
    encoded = urllib.parse.urlencode({k: v for k, v in params.items() if v is not None})
    sep = "&" if encoded else ""
    return f"{base_url}/api/debug/mock-ai-response{('?' + encoded) if encoded else ''}"


def provision_provider(base_url: str, headers: dict, name: str,
                       api_key: str = "tier-b-stub-key",
                       api_type: str = "API1") -> bool:
    """POST /api/settings/providers — creates a stub provider in the keystore.
    Tests reference it via key_name in the AI interface they create.  Returns
    True on success or 409 (already exists, idempotent on repeat runs)."""
    body = {
        "name": name,
        "display_name": f"[Tier B test] {name}",
        "endpoint": "https://localhost:8443/",
        "api_key": api_key,
        "api_type": api_type,
        "credential_type": "api_key",
    }
    r = requests.post(f"{base_url}/api/settings/providers",
                      json=body, headers=headers, verify=False, timeout=10)
    if r.status_code in (200, 201):
        return True
    if r.status_code == 409:
        # already exists — fine for repeated runs
        return True
    print(f"FAIL: provision_provider({name}) returned {r.status_code}: {r.text[:200]}")
    return False


def cleanup_provider(base_url: str, headers: dict, name: str) -> None:
    try:
        requests.delete(f"{base_url}/api/settings/providers/{name}",
                        headers=headers, verify=False, timeout=10)
    except Exception:
        pass


def provision_interface(base_url: str, headers: dict, *, name: str,
                        api_type: str, model: str, mock_url: str,
                        key_name: str,
                        rate_limit: dict | None = None) -> bool:
    """POST /api/settings/ai-interfaces — creates an AI interface pointing
    at the localhost mock endpoint.  `rate_limit` is the optional config
    block that drives the dispatcher controller (initial_concurrency_probe,
    max_concurrency, request_budget multipliers, etc.)."""
    body = {
        "name": name,
        "description": f"[Tier B test] {name}",
        "url": mock_url,
        "model": model,
        "api_type": api_type,
        "key_name": key_name,
    }
    if rate_limit:
        body["rate_limit"] = rate_limit
    r = requests.post(f"{base_url}/api/settings/ai-interfaces",
                      json=body, headers=headers, verify=False, timeout=10)
    if r.status_code in (200, 201):
        return True
    if r.status_code == 409:
        return True
    print(f"FAIL: provision_interface({name}) returned {r.status_code}: {r.text[:200]}")
    return False


def cleanup_interface(base_url: str, headers: dict, name: str) -> None:
    try:
        requests.delete(f"{base_url}/api/settings/ai-interfaces/{name}",
                        headers=headers, verify=False, timeout=10)
    except Exception:
        pass


def build_adhoc_jcwf(*, workflow_id: str, interface_name: str,
                     task_id: str = "echo",
                     prob_text: str = "Tier B Phase B test prompt.") -> dict:
    """Minimal one-task adhoc JCWF that fires one ai_call to the named
    interface.  Use prob_text to control the input-token estimate (chars/4)."""
    return {
        "version": "1.0",
        "id": workflow_id,
        "label": f"Tier B Phase B — {workflow_id}",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            task_id: {
                "id": task_id,
                "type": "ai_call",
                "label": "Tier B mock dispatch",
                "mode": "single",
                "working_directory": f"../../queue/{workflow_id}/01_{task_id}",
                "params": {"provider": interface_name},
                "queue_binding": {
                    "stng_files": [{"path": "STNG_x.txt", "content": "Tier B test."}],
                    "task_files": [{"path": "TASK_x.txt", "content": "No-op."}],
                    "cntx_files": [{"path": "CNTX_x.txt", "content": "Mock context."}],
                    "prob_files": [{"path": "PROB_x.txt", "content": prob_text}],
                },
            }
        },
    }


def submit_adhoc(base_url: str, headers: dict, jcwf: dict,
                 cleanup_policy: str = "ttl_1h") -> str | None:
    """POST /api/workflows/run-adhoc.  Returns runId on success, None on failure.
    Defaults to ttl_1h cleanup so the per-prob output files stay around for
    the test to inspect."""
    r = requests.post(f"{base_url}/api/workflows/run-adhoc",
                      json={"jcwf": jcwf, "cleanup_policy": cleanup_policy},
                      headers=headers, verify=False, timeout=30)
    if r.status_code not in (200, 202):
        print(f"FAIL: run-adhoc returned {r.status_code}: {r.text[:300]}")
        return None
    payload = r.json()
    return payload.get("runId") or payload.get("run_id")


def poll_run_state(base_url: str, headers: dict, run_id: str,
                   timeout_s: float = 30.0) -> str | None:
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
        time.sleep(0.2)
    return None


def get_signals(base_url: str, headers: dict) -> dict:
    r = requests.get(f"{base_url}/api/debug/signals",
                     headers=headers, verify=False, timeout=10)
    r.raise_for_status()
    return r.json().get("signals", {})


def get_recent_submissions(base_url: str, headers: dict) -> list[dict]:
    r = requests.get(f"{base_url}/api/debug/recent-submissions",
                     headers=headers, verify=False, timeout=10)
    r.raise_for_status()
    return r.json().get("submissions", [])


def get_run(base_url: str, headers: dict, run_id: str) -> dict | None:
    r = requests.get(f"{base_url}/api/workflow-runs/{run_id}",
                     headers=headers, verify=False, timeout=10)
    if r.status_code != 200:
        return None
    body = r.json()
    return body.get("run") if isinstance(body.get("run"), dict) else body


def find_controller(signals: dict, quota_key_substring: str) -> dict | None:
    """Look up a `dispatcher_controllers[]` entry whose quota_key contains
    the given substring.  Returns None if no match.  Tests use the host
    portion of quota_key (e.g. 'localhost') to match stable across model
    family variations."""
    for c in signals.get("dispatcher_controllers", []):
        if quota_key_substring in c.get("quota_key", ""):
            return c
    return None


def reset_dispatcher_state(base_url: str, headers: dict) -> bool:
    """POST /api/debug/reset-dispatcher-state — wipes controllers + host
    rate-limit state + recent-submissions ring.  Each Phase B test calls
    this at the very start so previous test runs in the same j9t process
    don't leak state into the new run."""
    try:
        r = requests.post(f"{base_url}/api/debug/reset-dispatcher-state",
                          headers=headers, verify=False, timeout=5)
        return r.status_code == 200 and r.json().get("ok", False)
    except Exception as e:
        print(f"WARN: reset_dispatcher_state failed: {e}")
        return False
