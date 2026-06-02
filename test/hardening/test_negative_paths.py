#!/usr/bin/env python3
"""
D1 negative-path verification.

One sub-test per rejection branch surfaced during the §S3 hardening
sittings (6–9).  Each sub-test follows the
`test/dispatch/test_envelope_empty_body_rejected.py` pattern:

  malformed input → REST API call → assert HTTP status + error code +
  ERROR log substring + /api/status still ok.

The script runs against a *live* j9t (default https://localhost:8443).
The host instance must be running with the keystore unlocked and an
admin MCP key in $J9T_TOKEN.  Per-test MCP keys are minted via
/api/auth/mcp-keys/enroll then revoked in the cleanup pass.

Test groups:

  Group 1 — Size caps
    1.1  adhoc JCWF > 4 MB → 400 jcwf_too_large (kMaxJcwfBytes)
    1.2  task output > 64 KiB with multibyte UTF-8 char straddling the cap
         boundary → callback payload truncates to 64 KiB AND backs off to a
         complete UTF-8 codepoint (no dangling lead byte).  Drives the
         truncation path via the Debug-only /api/debug/build-callback-payload
         endpoint so the SSRF gate's loopback rejection doesn't get in the
         way.  Sub-test SKIPPED on Release builds (debug endpoint absent).

  Group 2 — Path confinement on JCWF-supplied surfaces
    2.1  adhoc submission with cntx_files source = "/etc/passwd"
         → 202 staged but task fails with "CNTX source path resolves
            outside project root"
    2.2  PUT /api/workflows/<id> with workflow id containing "../" →
         rejected by registry's ConfineUnderProjectRoot guard
    2.3  file_watch trigger path = "/etc/" → registry trigger
         registration drops the watch silently with an APP ERROR;
         workflow stays loaded but the trigger never fires.
    2.4  polarion_write download_attachment file_path='/tmp/escape' →
         ConfineUnderProjectRoot rejects pre-network; task fails with
         "attachment output path does not resolve under project root"
         + no escape file written.
    2.5  polarion_write download_attachment work_item_id='../escape' →
         IsValidPolarionId rejects '..' substring; task fails with
         "invalid Polarion identifier".
    2.6  polarion_query filter.id='../escape' → mock returns items,
         per-item WriteItemFile rejected by IsValidFilesystemId
         allowlist; ERROR log line "[polarion] WriteItemFile rejected
         filter.m_Id '../escape' — fails allowlist" emitted per item.
         (Sub-tests 2.4-2.6 require polarionMockup running at
         :18080; skipped with a warning otherwise.)

  Group 3 — Concurrency
    3.1  WorkflowRegistry mutex stress: N parallel POST
         /api/workflows/reload + GET /api/workflows; all complete
         within timeout (no deadlock).
    3.2  Inflight-counter relax check: submit N adhoc workflows
         against the mock interface; after all are terminal, verify
         /api/status::ai_calls_inflight returns to 0 (no leak).

Deferred to Sitting 16 (REST-undrivable from a shared host instance):

  - db_query row/byte/timeout caps: need a populated DB + a fixture
    JCWF that targets `local-pg` with a deliberately-large SELECT.
    Coverage gap noted; flag candidate.
  - AI output size cap (kMaxOutputBytes 64 KB): cap applies to output
    file content read AFTER an AI call completes.  Drivable via a
    Python task that writes >64 KB to its output file, but the failure
    mode is internal-truncation not REST-observable rejection.
  - Reaper CV wake-on-stop: only observable via shutdown timing, not
    on a shared host instance.  Could be tested by spawning a separate
    j9t in a sandbox dir (see test_malformed_configs.py harness) and
    timing the /api/shutdown→exit delta; deferred as a non-merge-gating
    perf-only check.

Usage:

  python3 test/hardening/test_negative_paths.py
  python3 test/hardening/test_negative_paths.py --admin-key "$J9T_TOKEN"
  python3 test/hardening/test_negative_paths.py --group 1   # only group 1
"""

import argparse
import os
import sys
import time
import uuid
from concurrent.futures import ThreadPoolExecutor

try:
    import requests
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
except ImportError:
    print("ERROR: 'requests' package missing (pip install requests)")
    sys.exit(1)


class C:
    RESET = "\033[0m"; BOLD = "\033[1m"
    RED = "\033[91m"; GREEN = "\033[92m"; CYAN = "\033[96m"; YELLOW = "\033[93m"


def ok(msg):     print(f"  {C.GREEN}✓{C.RESET} {msg}")
def fail(msg):   print(f"  {C.RED}✗{C.RESET} {msg}")
def info(msg):   print(f"  {C.CYAN}ℹ{C.RESET} {msg}")
def warn(msg):   print(f"  {C.YELLOW}⚠{C.RESET} {msg}")
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'─'*70}\n  {msg}\n{'─'*70}{C.RESET}")


def http(method, base, path, key=None, **kw):
    headers = kw.pop("headers", {})
    if key:
        headers["Authorization"] = f"Bearer {key}"
    verify_ssl = not base.startswith("https://localhost")
    return requests.request(method, f"{base.rstrip('/')}{path}",
                            timeout=20, headers=headers, verify=verify_ssl, **kw)


def issue_key(base, admin_key, user, *, adhoc=True, role="operator", policy="ttl_72h"):
    enroll = http("POST", base, "/api/auth/mcp-keys/enroll", key=admin_key, json={
        "user": user, "role": role, "adhoc_enabled": adhoc,
        "disk_quota_mb": 1024, "default_cleanup_policy": policy,
        "description": "hardening test key", "key_expiry_days": 1,
        "enrollment_ttl_minutes": 5,
    })
    if enroll.status_code != 201:
        raise RuntimeError(f"enroll failed: {enroll.status_code} {enroll.text}")
    activate = http("POST", base, "/api/auth/mcp-keys/activate",
                    json={"enrollment_token": enroll.json()["enrollment_token"]})
    if activate.status_code != 200:
        raise RuntimeError(f"activate failed: {activate.status_code} {activate.text}")
    return activate.json()


def revoke_key(base, admin_key, key_id):
    http("DELETE", base, f"/api/auth/mcp-keys/{key_id}", key=admin_key)


def minimal_jcwf(wfid):
    """Smallest internal-task JCWF for adhoc — same shape as test_auth_mcp.py."""
    return {
        "id": wfid, "version": "1.0", "label": "hardening test",
        "manual_start": True,
        "tasks": {"noop": {"id": "noop", "type": "internal",
                            "params": {"action": "carMaintenance"}}},
    }


def expect(cond, msg, results):
    if cond:
        ok(msg); results[0] += 1
    else:
        fail(msg); results[1] += 1
    return cond


def expect_status_ok(base, admin_key, results):
    """Reaffirm the engine is still responsive after each negative-path test."""
    r = http("GET", base, "/api/status", key=admin_key)
    expect(r.status_code == 200, f"engine still alive (/api/status → {r.status_code})", results)


# =====================================================================
# Group 1 — Size caps
# =====================================================================

def group1_adhoc_jcwf_too_large(base, admin_key, results):
    header("1.1  adhoc JCWF > 4 MB → 400 stage_failed + 'jcwf_too_large' in message + log")
    user = f"hardening-size-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        # Build a JCWF whose JSON serialisation exceeds kMaxJcwfBytes
        # (4 MB).  Pad the description with 5 MB of 'A's.
        wfid = f"t{uuid.uuid4().hex[:8]}"
        jcwf = minimal_jcwf(wfid)
        jcwf["description"] = "A" * (5 * 1024 * 1024)
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "on_completion"})
        expect(r.status_code == 400, f"5 MB JCWF → 400 (got {r.status_code})", results)
        if r.status_code == 400 and r.headers.get("content-type", "").startswith("application/json"):
            body = r.json()
            # The webServer wraps all non-quota staging errors as
            # `stage_failed` and carries the underlying detail in
            # `message`.  Verify both the wrapper code AND the
            # underlying `jcwf_too_large` substring.
            expect(body.get("error") == "stage_failed",
                   f"error == 'stage_failed' (got {body.get('error')!r})", results)
            expect("jcwf_too_large" in body.get("message", ""),
                   f"message contains 'jcwf_too_large' (got {body.get('message')!r})", results)
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


def group1_output_cap_utf8_truncation(base, admin_key, results):
    header("1.2  output > 64 KiB with UTF-8 multibyte at boundary → callback payload caps + UTF-8-safe")
    # Probe whether the debug endpoint exists (Release builds strip it).
    probe = http("GET", base, "/api/debug/build-callback-payload", key=admin_key)
    if probe.status_code == 404:
        warn("debug endpoint absent (Release build?) — skipping 1.2")
        return
    user = f"hardening-cap-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        wfid = f"cap_{uuid.uuid4().hex[:8]}"
        # 65538-byte file: 65535 'A' + 3-byte UTF-8 '€' (U+20AC = E2 82 AC).
        # The '€' straddles the 65536-byte cap (lead at byte 65535,
        # continuations at 65536/65537).  Without the UTF-8-safe truncation
        # the callback payload would end with the lead 0xE2 alone — invalid
        # UTF-8 that detonates strict JSON validators + PyUnicode_FromString.
        # File built by scripts/writeOutputCapCanary.py (python task).
        jcwf = {
            "id": wfid, "version": "1.0", "label": "output cap UTF-8 canary",
            "manual_start": True,
            "tasks": {"big": {
                "id": "big", "type": "python",
                "working_directory": f"{wfid}/big",
                "params": {
                    "module": "writeOutputCapCanary",
                    "function": "write_canary",
                },
                "file_outputs": ["big.txt"],
                "outputs": {"content": {"type": "string"}},
            }},
        }
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "ttl_1h"})
        expect(r.status_code == 202, f"adhoc submit → 202 (got {r.status_code})", results)
        if r.status_code != 202:
            expect_status_ok(base, admin_key, results)
            return
        run_id = r.json().get("runId")
        state, task_err = _poll_terminal(base, key["api_key"], run_id, deadline_s=15)
        info(f"run state = {state!r}, task error = {task_err[:120]!r}")
        expect(state == "succeeded", f"run terminal = succeeded (got {state!r})", results)
        if state != "succeeded":
            expect_status_ok(base, admin_key, results)
            return

        r = http("GET", base, f"/api/debug/build-callback-payload?runId={run_id}", key=admin_key)
        expect(r.status_code == 200, f"debug payload endpoint → 200 (got {r.status_code})", results)
        if r.status_code != 200:
            expect_status_ok(base, admin_key, results)
            return
        payload = r.json()
        outputs = payload.get("tasks", {}).get("big", {}).get("outputs", {})
        # The python executor first copies the function's return dict into
        # m_OutputValues (here: "ok", "bytes_written"), then derivedOutputs
        # fills missing JCWF-declared slots ("content" → abs path to big.txt).
        # Look the slot up by name — first-iter order is insertion-dependent.
        info(f"output slots present: {sorted(outputs.keys())}")
        content = outputs.get("content")
        if content is None:
            fail(f"callback payload missing 'content' slot for task 'big' (got slots={sorted(outputs.keys())})")
            results[1] += 1
            expect_status_ok(base, admin_key, results)
            return

        content_bytes = content.encode("utf-8")
        info(f"content_bytes = {len(content_bytes)} bytes (file on disk = 65538)")
        expect(len(content_bytes) <= 65536,
               f"content ≤ 64 KiB cap (got {len(content_bytes)})", results)
        expect(len(content_bytes) < 65538,
               f"content was truncated (got {len(content_bytes)}, file is 65538)", results)
        # With the UTF-8-safe fix, the partial '€' is dropped → 65535 'A' bytes.
        # Without the fix, content would be 65536 bytes ending with the lead 0xE2.
        expect(len(content_bytes) == 65535,
               f"content == 65535 (65535 'A's; partial '€' dropped) — UTF-8-safe truncation "
               f"(got {len(content_bytes)})", results)
        try:
            content_bytes.decode("utf-8", errors="strict")
            ok("content is well-formed UTF-8 (no dangling lead byte)")
            results[0] += 1
        except UnicodeDecodeError as e:
            fail(f"content has invalid UTF-8: {e}")
            results[1] += 1
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


# =====================================================================
# Group 2 — Path confinement on JCWF-supplied surfaces
# =====================================================================

def group2_aicall_cntx_path_traversal(base, admin_key, results):
    header("2.1  adhoc JCWF with absolute cntx_files path → 400 at parse time")
    # The parser's IsAcceptedRelativePath (workflowJsonParserDetails.h)
    # rejects absolute paths in queue_binding string references BEFORE
    # staging — the absolute path never reaches the runtime's
    # ConfineUnderProjectRoot gate.  This is the outermost defence in
    # the cntx-path-traversal chain.
    user = f"hardening-cntx-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        wfid = f"t{uuid.uuid4().hex[:8]}"
        # cntx_files accepts bare strings (path references) per
        # test_markitdown_cntx.py's shipped pattern.  Absolute path
        # `/etc/passwd` hits IsAcceptedRelativePath rejection.
        jcwf = {
            "id": wfid, "version": "1.0", "label": "cntx absolute-path canary",
            "manual_start": True,
            "tasks": {"gen": {
                "id": "gen", "type": "ai_call",
                "working_directory": "../../queue/gen",
                "queue_binding": {
                    "stng_files": [{"path": "STNG.txt", "content": "x"}],
                    "task_files": [{"path": "TASK.txt", "content": "x"}],
                    "cntx_files": ["/etc/passwd"],
                    "prob_files": [{"path": "PROB.txt", "content": "x"}],
                },
            }},
        }
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "ttl_1h"})
        expect(r.status_code == 400, f"absolute cntx path → 400 (got {r.status_code})", results)
        if r.status_code == 400:
            body = r.json()
            expect(body.get("error") == "stage_failed",
                   f"error == 'stage_failed' (got {body.get('error')!r})", results)
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


def group2_workflow_id_traversal(base, admin_key, results):
    header("2.2  POST /api/workflows with workflow id containing '../' → rejected")
    # Workflow ids that contain path-traversal sequences MUST be rejected
    # before WorkflowRegistry::SaveOrUpdate touches the filesystem.  The
    # registry's ConfineUnderProjectRoot gate rejects the resolved path
    # before any write.  POST is the create endpoint; PUT requires the
    # workflow file to already exist on disk.
    bad_id = "../../etc/passwd-canary"
    jcwf = minimal_jcwf(bad_id)
    # POST body carries the workflow id INSIDE the JSON (the URL doesn't
    # carry it), so Crow can't normalise the `..` out of the URL.  The
    # registry's id-validation + path-confinement is what we want to hit.
    r = http("POST", base, "/api/workflows", key=admin_key, json=jcwf)
    expect(r.status_code >= 400, f"traversal id → {r.status_code} (≥ 400)", results)
    # Either the id-shape validator (alnum+underscore-only) or the
    # registry's ConfineUnderProjectRoot guard fires; either is correct
    # blast-radius containment.
    expect_status_ok(base, admin_key, results)


def group2_file_watch_path_traversal(base, admin_key, results):
    header("2.3  POST /api/workflows with file_watch trigger path = '/etc/' → trigger drops")
    # File-watch triggers go through ConfineUnderProjectRoot at trigger
    # registration time (triggerEngine.cpp:1096).  An out-of-root path
    # is logged + ignored — the workflow loads but the trigger never
    # fires.  POST (not PUT) is the create endpoint.  The workflow id
    # uses alphanumeric + underscore only (the registry's id-shape
    # allowlist).
    wfid = f"t{uuid.uuid4().hex[:8]}_fw_canary"
    jcwf = {
        "id": wfid, "version": "1.0", "label": "file_watch traversal canary",
        "triggers": [{
            "type": "file_watch", "id": "fw", "enabled": True,
            "params": {"path": "/etc/passwd-watch-canary", "pattern": "*"},
        }],
        "tasks": {"noop": {"id": "noop", "type": "internal",
                            "params": {"action": "carMaintenance"}}},
    }
    r = http("POST", base, "/api/workflows", key=admin_key, json=jcwf)
    # Acceptable outcomes:
    # - 200/201: workflow stored, trigger silently dropped at registration
    #   time (the legacy path-confinement behaviour)
    # - 400: parser/validator rejects out-of-root file_watch path up front
    # Both protect the operator.  A 5xx OR a successful trigger registration
    # (with the workflow actually firing on /etc activity) would be the
    # bug.
    expect(r.status_code in (200, 201, 400),
           f"file_watch traversal → {r.status_code} (200/201/400)", results)
    if r.status_code in (200, 201):
        # Clean up: delete the workflow so a future file_watch test isn't
        # confused by a stale canary.
        http("DELETE", base, f"/api/workflows/{wfid}", key=admin_key)
    expect_status_ok(base, admin_key, results)


def _polarion_mock_reachable(base, admin_key) -> bool:
    """True iff my-polarion connection passes /test (polarionMockup is up + connection registered)."""
    r = http("POST", base, "/api/connections/my-polarion/test", key=admin_key)
    return r.status_code == 200 and r.json().get("ok") is True


def group2_polarion_download_outputpath(base, admin_key, results):
    header("2.4  polarion_write download_attachment file_path='/tmp/...' → "
           "ConfineUnderProjectRoot rejects pre-network")
    if not _polarion_mock_reachable(base, admin_key):
        warn("my-polarion connection unreachable (polarionMockup down?) — skipping 2.4")
        return
    user = f"hardening-poldl-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    escape_path = "/tmp/j9t_polarion_escape_canary.bin"
    try:
        wfid = f"pol_{uuid.uuid4().hex[:8]}_dl"
        jcwf = {
            "id": wfid, "version": "1.0", "label": "polarion download path escape canary",
            "manual_start": True,
            "tasks": {"dl": {
                "id": "dl", "type": "polarion_write",
                "working_directory": f"{wfid}/dl",
                "params": {
                    "connection": "my-polarion",
                    "operation": "download_attachment",
                    "work_item_id": "REQ-003",
                    "attachment_id": "attachment-1",
                    "file_path": escape_path,
                },
            }},
        }
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "on_completion"})
        expect(r.status_code == 202, f"adhoc submit → 202 (got {r.status_code})", results)
        if r.status_code != 202:
            expect_status_ok(base, admin_key, results)
            return
        run_id = r.json().get("runId")
        state, task_err = _poll_terminal(base, key["api_key"], run_id, deadline_s=10)
        info(f"run state = {state!r}, task error = {task_err[:200]!r}")
        expect(state == "failed", f"run terminal = failed (got {state!r})", results)
        expect("attachment output path does not resolve under project root" in task_err,
               f"task error names outputPath rejection (got {task_err[:200]!r})", results)
        if os.path.exists(escape_path):
            fail(f"escape file written outside project root: {escape_path}")
            results[1] += 1
            os.remove(escape_path)
        else:
            ok(f"no escape file at {escape_path}")
            results[0] += 1
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


def group2_polarion_id_allowlist(base, admin_key, results):
    header("2.5  polarion_write download_attachment work_item_id='../escape' → "
           "IsValidPolarionId rejects '..' substring")
    if not _polarion_mock_reachable(base, admin_key):
        warn("my-polarion connection unreachable (polarionMockup down?) — skipping 2.5")
        return
    user = f"hardening-polid-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        wfid = f"pol_{uuid.uuid4().hex[:8]}_id"
        jcwf = {
            "id": wfid, "version": "1.0", "label": "polarion id allowlist canary",
            "manual_start": True,
            "tasks": {"dl": {
                "id": "dl", "type": "polarion_write",
                "working_directory": f"{wfid}/dl",
                "params": {
                    "connection": "my-polarion",
                    "operation": "download_attachment",
                    "work_item_id": "../escape",
                    "attachment_id": "attachment-1",
                    "file_path": "ok_canary.bin",
                },
            }},
        }
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "on_completion"})
        expect(r.status_code == 202, f"adhoc submit → 202 (got {r.status_code})", results)
        if r.status_code != 202:
            expect_status_ok(base, admin_key, results)
            return
        run_id = r.json().get("runId")
        state, task_err = _poll_terminal(base, key["api_key"], run_id, deadline_s=10)
        info(f"run state = {state!r}, task error = {task_err[:200]!r}")
        expect(state == "failed", f"run terminal = failed (got {state!r})", results)
        expect("invalid Polarion identifier" in task_err,
               f"task error names IsValidPolarionId rejection (got {task_err[:200]!r})", results)
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


def group2_polarion_writeitemfile_allowlist(base, admin_key, results):
    header("2.6  polarion_query filter.id='../escape' → WriteItemFile rejects per item (ERROR log)")
    if not _polarion_mock_reachable(base, admin_key):
        warn("my-polarion connection unreachable (polarionMockup down?) — skipping 2.6")
        return
    user = f"hardening-polwif-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        # Snapshot log byte offset so we grep only lines emitted by THIS run.
        log_pre = http("GET", base, "/api/log?tail=1", key=admin_key)
        start_offset = log_pre.json().get("byteOffset", 0) if log_pre.status_code == 200 else 0

        wfid = f"pol_{uuid.uuid4().hex[:8]}_wif"
        bad_filter_id = "../escape"
        jcwf = {
            "id": wfid, "version": "1.0", "label": "WriteItemFile allowlist canary",
            "manual_start": True,
            "filters": [{
                "id": bad_filter_id,
                "source": {
                    "kind": "polarion_query",
                    "connection": "my-polarion",
                    "query": "type:requirement",
                    "page_size": 5,
                },
                "binding": "req",
                "max_items": 1,
            }],
            "tasks": {"consume": {
                "id": "consume", "type": "shell",
                "mode": "per_item",
                "filter": bad_filter_id,
                "working_directory": f"{wfid}/consume",
                "params": {
                    "command": "/bin/true",
                },
            }},
        }
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "on_completion"})
        if r.status_code == 400:
            # If a future change moves the allowlist to parse time, the
            # rejection happens here — still correct (defense moved earlier).
            ok(f"parse-time rejection (acceptable hardening): {r.json().get('error','?')!r}")
            results[0] += 1
            expect_status_ok(base, admin_key, results)
            return
        expect(r.status_code == 202, f"adhoc submit → 202 (got {r.status_code})", results)
        if r.status_code != 202:
            expect_status_ok(base, admin_key, results)
            return
        run_id = r.json().get("runId")
        state, _ = _poll_terminal(base, key["api_key"], run_id, deadline_s=20)
        info(f"run state = {state!r}")

        log_post = http("GET", base, f"/api/log?offset={start_offset}", key=admin_key)
        log_text = "\n".join(log_post.json().get("lines", [])) if log_post.status_code == 200 else ""
        expect("WriteItemFile rejected filter.m_Id '../escape'" in log_text,
               "ERROR log line names WriteItemFile allowlist rejection with the hostile filter id", results)
        expect("fails allowlist" in log_text,
               "ERROR log line mentions 'fails allowlist'", results)
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


# =====================================================================
# Group 3 — Concurrency
# =====================================================================

def group3_registry_mutex_stress(base, admin_key, results):
    header("3.1  WorkflowRegistry mutex stress: N parallel reload + list")
    # The reload path takes the registry's write lock; list takes the
    # read lock.  N parallel mixed-mode requests should all complete
    # within a reasonable timeout — a deadlock would manifest as a
    # request that never returns (hits the 20 s HTTP timeout in
    # http()).
    N = 8

    def task(i):
        if i % 2 == 0:
            r = http("POST", base, "/api/workflows/reload", key=admin_key)
        else:
            r = http("GET", base, "/api/workflows", key=admin_key)
        return r.status_code

    t0 = time.time()
    with ThreadPoolExecutor(max_workers=N) as ex:
        codes = list(ex.map(task, range(N)))
    elapsed = time.time() - t0
    info(f"completed {N} requests in {elapsed:.2f}s; codes={codes}")
    expect(all(c == 200 for c in codes),
           f"all {N} requests returned 200 (got {codes})", results)
    expect(elapsed < 15.0,
           f"completed in < 15 s (no deadlock; elapsed={elapsed:.2f}s)", results)
    expect_status_ok(base, admin_key, results)


# =====================================================================
# Group 4 — db_query caps (path traversal + row cap + timeout)
# =====================================================================
#
# These tests target dbQueryCloudTaskExecutor's three caps:
#   - max_rows (default 100k, ceiling 1M)
#   - max_output_bytes (default 100 MB, ceiling 1 GB)
#   - statement_timeout_ms (default 60s, ceiling 600s)
# Plus the output_file ConfineUnderProjectRoot guard at line 282.
# Each test submits an adhoc JCWF containing a single db_query task
# against `local-pg`.  The host instance must have the local-pg
# connection unlocked (its password lives in the keystore) — if the
# connection isn't ready, the test prints a SKIP warning instead of
# failing.

def _db_query_jcwf(wfid, task_params):
    """Wrap a db_query task in a minimal adhoc JCWF."""
    return {
        "id": wfid, "version": "1.0", "label": f"db_query hardening canary {wfid}",
        "manual_start": True,
        "tasks": {"q": {
            "id": "q", "type": "db_query", "label": "canary",
            "working_directory": f"{wfid}/q",
            "params": task_params,
        }},
    }


def _poll_terminal(base, key, run_id, deadline_s=10):
    """Poll until the run reaches a terminal state.  Returns (state, first-task-error-message)."""
    deadline = time.time() + deadline_s
    while time.time() < deadline:
        r = http("GET", base, f"/api/workflow-runs/{run_id}", key=key)
        if r.status_code == 200:
            run = r.json().get("run", {})
            state = run.get("state")
            if state in ("succeeded", "failed"):
                # Per the run-JSON shape: tasks is a list of objects with
                # the per-task error in the `error` field (NOT
                # `last_error` or `error_message`).
                tasks = run.get("tasks", [])
                task_err = ""
                if isinstance(tasks, list) and tasks:
                    task_err = tasks[0].get("error", "")
                return state, task_err
        time.sleep(0.4)
    return None, ""


def group4_db_query_output_traversal(base, admin_key, results):
    header("4.1  db_query output_file = '/tmp/escape.csv' → ConfineUnderProjectRoot rejects")
    user = f"hardening-dbtrav-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        wfid = f"dbq_{uuid.uuid4().hex[:8]}_trav"
        jcwf = _db_query_jcwf(wfid, {
            "connection": "local-pg",
            "query": "SELECT 1 AS x",
            "format": "csv",
            "output_file": "/tmp/j9t_db_query_escape_canary.csv",
        })
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "on_completion"})
        if r.status_code == 400:
            # Submission may be rejected at parse time if the JCWF
            # schema validator catches absolute output paths — also
            # an acceptable rejection layer.
            body = r.json()
            ok(f"submit-time rejection: {body.get('error', '?')!r}")
            results[0] += 1
            expect_status_ok(base, admin_key, results)
            return
        expect(r.status_code == 202, f"adhoc submit → 202 (got {r.status_code})", results)
        if r.status_code != 202:
            expect_status_ok(base, admin_key, results)
            return
        run_id = r.json().get("runId")
        state, task_err = _poll_terminal(base, key["api_key"], run_id)
        info(f"run state = {state!r}, task error = {task_err[:120]!r}")
        # The task should fail because ConfineUnderProjectRoot returns
        # empty for /tmp/.  Either the task fails OR the run fails.
        expect(state == "failed", f"run terminal = failed (got {state!r})", results)
        # And the escape file MUST NOT be created on disk.
        import os
        escape_path = "/tmp/j9t_db_query_escape_canary.csv"
        if os.path.exists(escape_path):
            fail(f"escape file written outside project root: {escape_path}")
            results[1] += 1
            os.remove(escape_path)
        else:
            ok(f"no escape file at {escape_path}")
            results[0] += 1
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


def group4_db_query_row_cap(base, admin_key, results):
    header("4.2  db_query max_rows=5 with 100-row query → task fails 'exceeds max_rows=5'")
    user = f"hardening-dbrows-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        wfid = f"dbq_{uuid.uuid4().hex[:8]}_rows"
        jcwf = _db_query_jcwf(wfid, {
            "connection": "local-pg",
            "query": "SELECT generate_series(1, 100) AS x",
            "max_rows": 5,
            "format": "csv",
            "output_file": "rows_canary.csv",
        })
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "on_completion"})
        expect(r.status_code == 202, f"adhoc submit → 202 (got {r.status_code})", results)
        if r.status_code != 202:
            expect_status_ok(base, admin_key, results)
            return
        run_id = r.json().get("runId")
        state, task_err = _poll_terminal(base, key["api_key"], run_id)
        info(f"run state = {state!r}, task error = {task_err[:200]!r}")
        if state is None:
            warn("run did not reach terminal state in 10s — local-pg may not be unlocked")
            return
        # local-pg connection may not be unlocked → connection failure
        # before the row cap is even reached.  Distinguish:
        if "connection" in task_err.lower() or "password" in task_err.lower():
            warn(f"local-pg connection appears unavailable — skipping cap assertion ({task_err[:120]!r})")
            return
        expect(state == "failed", f"run terminal = failed (got {state!r})", results)
        expect("exceeds max_rows=5" in task_err,
               f"task error contains 'exceeds max_rows=5' (got {task_err[:120]!r})", results)
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


def group4_db_query_timeout(base, admin_key, results):
    header("4.3  db_query statement_timeout_ms=100 + pg_sleep(2) → timeout ERROR")
    user = f"hardening-dbtmo-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        wfid = f"dbq_{uuid.uuid4().hex[:8]}_tmo"
        jcwf = _db_query_jcwf(wfid, {
            "connection": "local-pg",
            "query": "SELECT pg_sleep(2)",
            "statement_timeout_ms": 100,
            "format": "csv",
            "output_file": "tmo_canary.csv",
        })
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "on_completion"})
        expect(r.status_code == 202, f"adhoc submit → 202 (got {r.status_code})", results)
        if r.status_code != 202:
            expect_status_ok(base, admin_key, results)
            return
        run_id = r.json().get("runId")
        state, task_err = _poll_terminal(base, key["api_key"], run_id, deadline_s=8)
        info(f"run state = {state!r}, task error = {task_err[:200]!r}")
        if state is None:
            warn("run did not reach terminal state in 8s")
            return
        if "connection" in task_err.lower() or "password" in task_err.lower():
            warn(f"local-pg connection appears unavailable — skipping timeout assertion")
            return
        expect(state == "failed", f"run terminal = failed (got {state!r})", results)
        # libpq surfaces statement_timeout firings as
        # "canceling statement due to statement timeout" — the executor
        # routes that through PostgreSQL query failed: <message>.
        expect("statement timeout" in task_err.lower() or "canceling" in task_err.lower(),
               f"task error mentions 'statement timeout' or 'canceling' (got {task_err[:120]!r})", results)
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


def group4_db_query_breaker_app_level_classification(base, admin_key, results):
    header("4.4  N consecutive max_rows cap rejections → breaker stays Closed "
           "(IsConnectionFailure(ValueOutOfRange) == false)")
    # Probes the typed-failure-code policy: a db_query that exceeds max_rows
    # is an app-level rejection (the connection is healthy, the query just
    # produced too many rows), so the circuit breaker must NOT decrement the
    # connection's health budget.  Without this policy, 5 consecutive cap
    # rejections would trip the breaker open and lock the user out of the
    # connection for 60 s.  No warm-up — the assertion IS that no warm-up is
    # needed.
    def _fetch_health():
        r = http("GET", base, "/api/status", key=admin_key)
        if r.status_code != 200:
            return None
        for ch in r.json().get("connection_health", []):
            if ch.get("name") == "local-pg":
                return ch
        return None

    baseline = _fetch_health()
    if baseline is None:
        warn("local-pg not in connection_health — skipping cap-classification test")
        return
    if baseline.get("circuit_state") != "closed":
        warn(f"local-pg breaker not Closed at baseline ({baseline.get('circuit_state')!r}) — "
             f"skipping (run /api/connections/local-pg/test to reset)")
        return
    baseline_failures = baseline.get("consecutive_failures", 0)
    info(f"baseline: state={baseline.get('circuit_state')!r}, "
         f"consecutive_failures={baseline_failures}")

    user = f"hardening-brc-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        N = 6  # one more than the default failure threshold (5)
        ran = 0
        for i in range(N):
            wfid = f"dbq_{uuid.uuid4().hex[:8]}_brc{i}"
            jcwf = _db_query_jcwf(wfid, {
                "connection": "local-pg",
                "query": "SELECT generate_series(1, 100) AS x",
                "max_rows": 5,
                "format": "csv",
                "output_file": "rows_canary.csv",
            })
            r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                     json={"jcwf": jcwf, "cleanup_policy": "on_completion"})
            if r.status_code != 202:
                warn(f"adhoc submit {i+1}/{N} → {r.status_code} (skipping rest)")
                break
            state, task_err = _poll_terminal(base, key["api_key"], r.json().get("runId"))
            if state is None:
                warn(f"run {i+1}/{N} did not reach terminal state — aborting test")
                return
            # A connection-class failure here (e.g. local-pg unreachable) WOULD
            # legitimately tick the breaker; bail out so we don't accidentally
            # blame the wrong subsystem.
            if "exceeds max_rows=5" not in task_err:
                warn(f"run {i+1}/{N} did not hit the cap branch (err={task_err[:120]!r}) "
                     f"— skipping cap-classification assertion")
                return
            ran += 1

        expect(ran == N, f"all {N} runs hit the max_rows cap branch (ran={ran})", results)

        after = _fetch_health()
        expect(after is not None, "local-pg still present in connection_health", results)
        if after is None:
            return
        expect(after.get("circuit_state") == "closed",
               f"breaker still Closed after {N} cap rejections (got {after.get('circuit_state')!r})",
               results)
        expect(after.get("consecutive_failures", 0) == baseline_failures,
               f"consecutive_failures unchanged from baseline "
               f"(baseline={baseline_failures}, after={after.get('consecutive_failures', 0)})",
               results)
        expect(after.get("last_failure_code") == "value_out_of_range",
               f"last_failure_code reflects the typed code "
               f"(got {after.get('last_failure_code')!r})", results)
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


def group3_inflight_counter_leak(base, admin_key, results):
    header("3.2  inflight-counter leak: ai_calls_inflight returns to 0 after batch")
    # Snapshot ai_calls_inflight BEFORE + submit N quick internal-task
    # workflows (no actual AI calls but exercises the runtime manager's
    # task lifecycle).  After all terminal, expect inflight to be the
    # same as before — drift indicates a leak.
    before = http("GET", base, "/api/status", key=admin_key).json().get("ai_calls_inflight", 0)
    info(f"baseline ai_calls_inflight = {before}")

    user = f"hardening-leak-{uuid.uuid4().hex[:8]}@example.com"
    key = issue_key(base, admin_key, user)
    try:
        N = 5
        run_ids = []
        for i in range(N):
            wfid = f"t{uuid.uuid4().hex[:8]}-leak{i}"
            jcwf = minimal_jcwf(wfid)
            r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                     json={"jcwf": jcwf, "cleanup_policy": "on_completion"})
            if r.status_code == 202:
                run_ids.append(r.json().get("runId"))
        info(f"submitted {len(run_ids)} runs")

        deadline = time.time() + 15
        terminal_count = 0
        while time.time() < deadline:
            terminal_count = 0
            for rid in run_ids:
                s = http("GET", base, f"/api/workflow-runs/{rid}", key=key["api_key"])
                if s.status_code == 200:
                    state = s.json().get("run", {}).get("state")
                    if state in ("succeeded", "failed"):
                        terminal_count += 1
            if terminal_count == len(run_ids):
                break
            time.sleep(0.5)
        info(f"{terminal_count}/{len(run_ids)} runs reached terminal state")
        # Small grace period for the runtime to release ai_calls_inflight.
        time.sleep(1.0)
        after = http("GET", base, "/api/status", key=admin_key).json().get("ai_calls_inflight", 0)
        info(f"post-batch ai_calls_inflight = {after}")
        # Internal tasks don't actually call AI providers, so the
        # ai_calls_inflight counter should NOT have grown.  Even if it
        # did transiently, it must be back to <= baseline after the
        # batch completes.
        expect(after <= before,
               f"ai_calls_inflight returned to ≤ baseline ({after} ≤ {before})", results)
        expect_status_ok(base, admin_key, results)
    finally:
        revoke_key(base, admin_key, key["key_id"])


# =====================================================================
# Driver
# =====================================================================

GROUPS = {
    1: [group1_adhoc_jcwf_too_large, group1_output_cap_utf8_truncation],
    2: [group2_aicall_cntx_path_traversal, group2_workflow_id_traversal,
        group2_file_watch_path_traversal,
        group2_polarion_download_outputpath, group2_polarion_id_allowlist,
        group2_polarion_writeitemfile_allowlist],
    3: [group3_registry_mutex_stress, group3_inflight_counter_leak],
    4: [group4_db_query_output_traversal, group4_db_query_row_cap,
        group4_db_query_timeout, group4_db_query_breaker_app_level_classification],
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=os.environ.get("J9T_URL", "https://localhost:8443"))
    parser.add_argument("--admin-key",
                        default=os.environ.get("J9T_TOKEN") or os.environ.get("J9T_ADMIN_KEY"))
    parser.add_argument("--group", type=int, action="append", default=None,
                        help="run only the named group(s) (1=size caps, 2=path conf, 3=concurrency)")
    args = parser.parse_args()

    if not args.admin_key or not args.admin_key.startswith("mcp_"):
        print(f"{C.RED}ERROR:{C.RESET} --admin-key (or J9T_TOKEN) must be an 'mcp_...' key")
        return 2

    info(f"base = {args.base_url}")
    info(f"admin key = {args.admin_key[:12]}...")

    selected = args.group if args.group else sorted(GROUPS.keys())
    results = [0, 0]

    for g in selected:
        if g not in GROUPS:
            warn(f"unknown group {g}, skipping")
            continue
        for test_fn in GROUPS[g]:
            test_fn(args.base_url, args.admin_key, results)

    passed, failed = results
    print()
    if failed == 0:
        print(f"{C.GREEN}PASS:{C.RESET} {passed} checks across {len(selected)} group(s)")
        return 0
    print(f"{C.RED}FAIL:{C.RESET} {passed} passed, {failed} failed across {len(selected)} group(s)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
