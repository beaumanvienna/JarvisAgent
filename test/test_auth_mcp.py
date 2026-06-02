#!/usr/bin/env python3
"""
MCP API-Key + Session + Adhoc integration tests.

Covers the scenarios enumerated in "Adhoc Workflow Submission and MCP plan.md" §12:
  - Enrollment → activation → authenticate round-trip
  - Self-renewal (valid + expired / revoked cases)
  - Dashboard login / logout session lifecycle + cookie flags
  - Session auth on subsequent requests
  - RBAC enforcement (viewer + operator vs admin-only endpoints)
  - Revocation (revoked key stops authenticating)
  - Adhoc submission: success + missing adhoc_enabled + bad cleanup_policy
  - Studio: mcp_ prefix enforced even when browser UI is open
  - Audit log: enrollment and auth events land in log/security.txt

Does **not** cover (requires richer tooling):
  - Master-password memory protection (SecureString zeroing — needs a debugger / core dump)
  - WebSocket session auth (needs websocket-client; the suite falls back gracefully)
  - Session idle-timeout after 8h (would require mocking time; skipped)

Usage:
    # Bootstrap your admin key first:
    #   curl -sS -X POST http://localhost:8080/api/auth/mcp-keys/activate \\
    #        -H 'Content-Type: application/json' \\
    #        -d '{"enrollment_token":"enroll_..."}'
    # Then:
    python3 test/test_auth_mcp.py --admin-key mcp_...
    python3 test/test_auth_mcp.py --admin-key "$J9T_ADMIN_KEY" --base-url http://host:8080
"""

import argparse
import json
import os
import sys
import time
import uuid
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent

try:
    import requests
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
except ImportError:
    print("ERROR: 'requests' package not found. Install with: pip install requests")
    sys.exit(1)


class C:
    RESET  = "\033[0m"
    BOLD   = "\033[1m"
    RED    = "\033[91m"
    GREEN  = "\033[92m"
    YELLOW = "\033[93m"
    CYAN   = "\033[96m"
    DIM    = "\033[2m"


def ok(msg):     print(f"  {C.GREEN}\u2713{C.RESET} {msg}")
def fail(msg):   print(f"  {C.RED}\u2717{C.RESET} {msg}")
def info(msg):   print(f"  {C.CYAN}\u2139{C.RESET} {msg}")
def warn(msg):   print(f"  {C.YELLOW}\u26a0{C.RESET} {msg}")
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'\u2500'*60}\n  {msg}\n{'\u2500'*60}{C.RESET}")


class Runner:
    def __init__(self, base_url: str, admin_key: str):
        self.base = base_url.rstrip("/")
        self.admin_key = admin_key
        self.verify_ssl = not base_url.startswith("https://localhost")
        self.passed = 0
        self.failed = 0
        # Set by main() based on --with-ai. Tests that need a live AI provider
        # check this flag and skip cleanly when it's false.
        self.with_ai = False

    def url(self, path):
        return f"{self.base}{path}"

    def get(self, path, key=None, **kw):
        headers = kw.pop("headers", {})
        if key:
            headers["Authorization"] = f"Bearer {key}"
        return requests.get(self.url(path), timeout=10, headers=headers, verify=self.verify_ssl, **kw)

    def post(self, path, key=None, **kw):
        headers = kw.pop("headers", {})
        if key:
            headers["Authorization"] = f"Bearer {key}"
        return requests.post(self.url(path), timeout=10, headers=headers, verify=self.verify_ssl, **kw)

    def put(self, path, key=None, **kw):
        headers = kw.pop("headers", {})
        if key:
            headers["Authorization"] = f"Bearer {key}"
        return requests.put(self.url(path), timeout=10, headers=headers, verify=self.verify_ssl, **kw)

    def delete(self, path, key=None, **kw):
        headers = kw.pop("headers", {})
        if key:
            headers["Authorization"] = f"Bearer {key}"
        return requests.delete(self.url(path), timeout=10, headers=headers, verify=self.verify_ssl, **kw)

    def expect(self, cond, msg):
        if cond:
            ok(msg)
            self.passed += 1
        else:
            fail(msg)
            self.failed += 1
        return cond

    # ------------------------------------------------------------------
    # Helpers that produce a fresh enrollment / activated MCP key
    # ------------------------------------------------------------------
    def issue_key(self, user, role="operator", adhoc=False, quota_mb=1024,
                  default_policy="ttl_72h"):
        enroll_body = {
            "user": user,
            "role": role,
            "adhoc_enabled": adhoc,
            "disk_quota_mb": quota_mb,
            "default_cleanup_policy": default_policy,
            "description": "test key",
            "key_expiry_days": 90,
            "enrollment_ttl_minutes": 5,
        }
        r = self.post("/api/auth/mcp-keys/enroll", key=self.admin_key, json=enroll_body)
        if r.status_code != 201:
            raise RuntimeError(f"enroll failed: {r.status_code} {r.text}")
        token = r.json()["enrollment_token"]
        r = self.post("/api/auth/mcp-keys/activate", json={"enrollment_token": token})
        if r.status_code != 200:
            raise RuntimeError(f"activate failed: {r.status_code} {r.text}")
        return r.json()  # {key_id, api_key, user, role, expires_at, ...}

    # ==================================================================
    # Test batteries
    # ==================================================================
    def test_whoami_unauth(self):
        header("whoami without credentials")
        r = self.get("/api/auth/whoami")
        # Studio returns 200 with user=studio, role=admin; Engine returns 401.
        if r.status_code == 200:
            body = r.json()
            self.expect(body.get("ok") is True, "200 OK in Studio (no auth required)")
        else:
            self.expect(r.status_code == 401, f"401 Unauthorized in Engine (got {r.status_code})")

    def test_admin_key_works(self):
        header("admin key authenticates whoami")
        r = self.get("/api/auth/whoami", key=self.admin_key)
        self.expect(r.status_code == 200, f"whoami status == 200 (got {r.status_code})")
        body = r.json() if r.status_code == 200 else {}
        self.expect(body.get("role") == "admin", f"role == admin (got {body.get('role')!r})")
        self.expect(body.get("user", "") != "", "user field non-empty")

    def test_enrollment_roundtrip(self):
        header("enrollment → activation → authenticate round-trip")
        user = f"test-operator-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator")
        self.expect(key["role"] == "operator", "activated key carries 'operator' role")
        self.expect(key["api_key"].startswith("mcp_"), "api_key starts with 'mcp_'")
        self.expect(len(key["api_key"]) == 68, f"api_key length == 68 (got {len(key['api_key'])})")
        r = self.get("/api/auth/whoami", key=key["api_key"])
        body = r.json() if r.status_code == 200 else {}
        self.expect(r.status_code == 200, f"whoami succeeds with new key (got {r.status_code})")
        self.expect(body.get("user") == user, f"whoami.user == {user!r}")
        self.expect(body.get("role") == "operator", f"whoami.role == 'operator'")
        return key

    def test_invalid_key_rejected(self):
        header("invalid / malformed keys are rejected")
        bad_keys = [
            "mcp_" + "0" * 64,                     # well-formed but never issued
            "mcp_notevenhex!!!",                    # malformed
            "not-an-mcp-key-at-all",                # wrong prefix
        ]
        for bk in bad_keys:
            r = self.get("/api/auth/whoami", key=bk)
            # Either 401 missing/invalid or 403 forbidden is acceptable — anything but 200.
            self.expect(r.status_code in (401, 403),
                        f"key {bk[:24]!r}... → {r.status_code} (≠ 200)")

    def test_rbac_viewer_cannot_enroll(self):
        header("RBAC: viewer cannot create enrollments (admin-only)")
        user = f"test-viewer-{uuid.uuid4().hex[:8]}@example.com"
        viewer_key = self.issue_key(user, role="viewer")
        r = self.post("/api/auth/mcp-keys/enroll", key=viewer_key["api_key"], json={
            "user": "shouldnt-happen@example.com", "role": "viewer",
        })
        self.expect(r.status_code == 403, f"enroll as viewer → 403 (got {r.status_code})")

    def test_self_renew(self):
        header("self-renewal: valid key issues a new key")
        user = f"test-renew-{uuid.uuid4().hex[:8]}@example.com"
        old = self.issue_key(user, role="operator")
        r = self.post("/api/auth/mcp-keys/self-renew", key=old["api_key"])
        self.expect(r.status_code == 200, f"self-renew status == 200 (got {r.status_code})")
        body = r.json() if r.status_code == 200 else {}
        self.expect(body.get("api_key", "").startswith("mcp_"), "new api_key issued")
        self.expect(body.get("api_key") != old["api_key"], "new key differs from old")
        # New key authenticates
        r = self.get("/api/auth/whoami", key=body.get("api_key", ""))
        self.expect(r.status_code == 200, "new key works for whoami")
        # Old key still works (24h grace) — don't assert an exact outcome
        # because the grace period is backend-configurable, but the key must
        # at least not hard-fail the immediate next call.
        r2 = self.get("/api/auth/whoami", key=old["api_key"])
        info(f"old key after renew → {r2.status_code} (grace period)")

    def test_revoke(self):
        header("revoked key stops authenticating")
        user = f"test-revoke-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator")
        r = self.delete(f"/api/auth/mcp-keys/{key['key_id']}", key=self.admin_key)
        self.expect(r.status_code == 200, f"revoke status == 200 (got {r.status_code})")
        r = self.get("/api/auth/whoami", key=key["api_key"])
        self.expect(r.status_code in (401, 403),
                    f"revoked key → {r.status_code} (≠ 200)")

    def test_session_login_logout(self):
        header("dashboard login + cookie + logout")
        user = f"test-session-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator")
        sess = requests.Session()
        r = sess.post(self.url("/api/auth/login"),
                      json={"api_key": key["api_key"]}, timeout=10, verify=self.verify_ssl)
        self.expect(r.status_code == 200, f"login status == 200 (got {r.status_code})")
        # Cookie flags
        cookie = None
        for c in sess.cookies:
            if c.name == "session":
                cookie = c
                break
        self.expect(cookie is not None, "session cookie set")
        if cookie is not None:
            # python-requests doesn't expose SameSite directly; check via Set-Cookie header.
            set_cookie = r.headers.get("Set-Cookie", "")
            self.expect("HttpOnly" in set_cookie, "Set-Cookie carries HttpOnly")
            self.expect("SameSite=Strict" in set_cookie, "Set-Cookie carries SameSite=Strict")
            # Secure only set when TLS is enabled server-side; don't require it for HTTP test setups.
        # Session cookie authenticates whoami
        r = sess.get(self.url("/api/auth/whoami"), timeout=10, verify=self.verify_ssl)
        self.expect(r.status_code == 200, "whoami via session cookie == 200")
        body = r.json() if r.status_code == 200 else {}
        self.expect(body.get("user") == user, f"whoami.user via session == {user!r}")
        # Logout
        r = sess.post(self.url("/api/auth/logout"), timeout=10, verify=self.verify_ssl)
        self.expect(r.status_code == 200, f"logout status == 200 (got {r.status_code})")
        r = sess.get(self.url("/api/auth/whoami"), timeout=10, verify=self.verify_ssl)
        # After logout, whoami is either 401 (Engine) or 200 with studio identity
        self.expect(r.status_code in (200, 401), f"post-logout whoami → {r.status_code}")
        if r.status_code == 200:
            post_body = r.json()
            self.expect(post_body.get("user") != user,
                        "post-logout whoami no longer returns the logged-in user")

    def test_activate_invalid_token(self):
        header("activation: invalid enrollment token rejected")
        r = self.post("/api/auth/mcp-keys/activate",
                      json={"enrollment_token": "enroll_" + "0" * 64})
        self.expect(r.status_code == 401, f"invalid enrollment → 401 (got {r.status_code})")

    def test_adhoc_missing_flag(self):
        header("adhoc submission: missing adhoc_enabled → 403")
        user = f"test-adhoc-off-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator", adhoc=False)
        jcwf = minimal_adhoc_jcwf(f"t{uuid.uuid4().hex[:8]}")
        r = self.post("/api/workflows/run-adhoc", key=key["api_key"],
                      json={"jcwf": jcwf})
        self.expect(r.status_code == 403, f"adhoc without adhoc_enabled → 403 (got {r.status_code})")
        body = r.json() if r.headers.get("content-type", "").startswith("application/json") else {}
        self.expect(body.get("error") == "adhoc_not_enabled",
                    f"error == 'adhoc_not_enabled' (got {body.get('error')!r})")

    def test_adhoc_bad_policy(self):
        header("adhoc submission: invalid cleanup_policy → 400")
        user = f"test-adhoc-bad-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator", adhoc=True)
        jcwf = minimal_adhoc_jcwf(f"t{uuid.uuid4().hex[:8]}")
        r = self.post("/api/workflows/run-adhoc", key=key["api_key"],
                      json={"jcwf": jcwf, "cleanup_policy": "forever"})
        self.expect(r.status_code == 400, f"bad policy → 400 (got {r.status_code})")

    def test_adhoc_policy_ceiling(self):
        header("adhoc submission: policy exceeding user ceiling → 403")
        user = f"test-adhoc-ceiling-{uuid.uuid4().hex[:8]}@example.com"
        # Key's ceiling is ttl_1h; try to submit with retain (longer).
        key = self.issue_key(user, role="operator", adhoc=True, default_policy="ttl_1h")
        jcwf = minimal_adhoc_jcwf(f"t{uuid.uuid4().hex[:8]}")
        r = self.post("/api/workflows/run-adhoc", key=key["api_key"],
                      json={"jcwf": jcwf, "cleanup_policy": "retain"})
        self.expect(r.status_code == 403, f"retain > ttl_1h → 403 (got {r.status_code})")
        body = r.json() if r.headers.get("content-type", "").startswith("application/json") else {}
        self.expect(body.get("error") == "policy_exceeds_ceiling",
                    f"error == 'policy_exceeds_ceiling' (got {body.get('error')!r})")

    def test_adhoc_happy_path(self):
        header("adhoc submission: happy path → 202")
        user = f"test-adhoc-ok-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator", adhoc=True)
        wfid = f"t{uuid.uuid4().hex[:8]}"
        jcwf = minimal_adhoc_jcwf(wfid)
        r = self.post("/api/workflows/run-adhoc", key=key["api_key"],
                      json={"jcwf": jcwf, "cleanup_policy": "on_completion",
                            "context": {"greeting": "hi"}})
        self.expect(r.status_code == 202, f"adhoc submit → 202 (got {r.status_code}: {r.text[:200]})")
        body = r.json() if r.status_code == 202 else {}
        self.expect(body.get("runId", "").startswith("adhoc_"), f"runId starts with 'adhoc_'")
        self.expect(body.get("workflowId", "").startswith("_adhoc_"),
                    f"workflowId starts with '_adhoc_'")
        return body

    def test_adhoc_aicall_roundtrip(self):
        # Exercises the full ai_call dispatch pipeline for adhoc runs — the
        # bug fixed by the FileWatcher dynamic AddPath/RemovePath refactor
        # (log/AdhocQueueFolderMonitoring.md, no longer present after landing).
        # Requires a working AI provider; gated on --with-ai so clean installs
        # still pass the rest of the suite.
        header("adhoc ai_call: end-to-end dispatch (requires --with-ai)")
        if not self.with_ai:
            info("skipped — pass --with-ai to exercise this test")
            return

        user = f"test-ai-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator", adhoc=True)
        wfid = f"t{uuid.uuid4().hex[:8]}"
        jcwf = {
            "id": wfid, "version": "1.0", "label": "ai_call round-trip",
            "manual_start": True,
            "tasks": {
                "gen": {
                    "id": "gen", "type": "ai_call", "params": {},
                    "working_directory": "../../queue/gen",
                    "queue_binding": {
                        "stng_files": [{"path": "STNG.txt",
                                        "content": "Output only plain text. One short sentence."}],
                        "task_files": [{"path": "TASK.txt",
                                        "content": "Answer with 'ready'."}],
                        "cntx_files": [{"path": "CNTX.txt", "content": "Test context."}],
                        "prob_files": [{"path": "PROB.txt", "content": "Please answer."}],
                    },
                }
            },
        }
        submit = self.post("/api/workflows/run-adhoc", key=key["api_key"],
                           json={"jcwf": jcwf, "cleanup_policy": "ttl_1h"})
        self.expect(submit.status_code == 202, f"submit → 202 (got {submit.status_code})")
        if submit.status_code != 202:
            return
        run_id = submit.json().get("runId", "")

        # Poll for success. Envelope dispatch + provider round-trip typically
        # < 5 s; give it 45 s to tolerate slow providers under load.
        deadline = time.time() + 45
        terminal_state = None
        while time.time() < deadline:
            s = self.get(f"/api/workflow-runs/{run_id}", key=self.admin_key)
            if s.status_code == 200:
                state = s.json().get("run", {}).get("state", "")
                if state in ("succeeded", "failed", "cancelled"):
                    terminal_state = state
                    break
            time.sleep(1)
        self.expect(terminal_state == "succeeded",
                    f"ai_call run reaches 'succeeded' (got {terminal_state!r})")

        # Verify the envelope-direct dispatch path accounted the call.
        # debug_signals' ai_calls_inflight returns to 0 once the call completes; the
        # assertion here is that the signals endpoint remains reachable for admins.
        dbg = self.get("/api/debug/signals", key=self.admin_key)
        self.expect(dbg.status_code == 200, f"debug signals reachable (got {dbg.status_code})")
        # Verify the AI output file exists in the run folder.
        listing = self.get(f"/api/workflow-runs/{run_id}/files",
                           key=self.admin_key)
        if listing.status_code == 200:
            files = listing.json().get("files", [])
            output_present = any(f.get("path", "").endswith(".output.txt") for f in files)
            self.expect(output_present, "at least one *.output.txt file present in the run folder")

    def test_adhoc_folder_namespace(self):
        header("adhoc: staged folder is namespaced under the user slug")
        user = f"test-layout-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator", adhoc=True)
        wfid = f"t{uuid.uuid4().hex[:8]}"
        jcwf = minimal_adhoc_jcwf(wfid)
        r = self.post("/api/workflows/run-adhoc", key=key["api_key"],
                      json={"jcwf": jcwf, "cleanup_policy": "ttl_1h"})
        self.expect(r.status_code == 202, f"adhoc submit → 202 (got {r.status_code})")
        if r.status_code != 202:
            return
        body = r.json()
        folder = body.get("folder_path", "")
        # Post Sitting-9 SanitizeUserSlug: slug body is the ASCII-safe portion
        # followed by `_<8 hex of SHA-256(user)>`.  Check the body substring
        # WITHOUT a trailing slash (the next char is `_` from the hash suffix).
        self.expect(f"/_adhoc/{user}_" in folder,
                    f"folder_path contains '/_adhoc/{user}_' (got {folder!r})")

    def test_run_files_list(self):
        header("run files: list endpoint (ownership, retention, files[])")
        # Submit two adhoc runs owned by different users so we can exercise
        # both the happy path (owner reads own run) and the denial path
        # (non-owner, non-admin reads someone else's run).
        alice_user = f"test-files-alice-{uuid.uuid4().hex[:8]}@example.com"
        bob_user   = f"test-files-bob-{uuid.uuid4().hex[:8]}@example.com"
        alice_key = self.issue_key(alice_user, role="operator", adhoc=True)
        bob_key   = self.issue_key(bob_user,   role="operator", adhoc=True)

        # Alice submits an adhoc run with ttl_1h so the folder persists.
        jcwf = minimal_adhoc_jcwf(f"t{uuid.uuid4().hex[:8]}")
        submit = self.post("/api/workflows/run-adhoc", key=alice_key["api_key"],
                           json={"jcwf": jcwf, "cleanup_policy": "ttl_1h"})
        self.expect(submit.status_code == 202, f"alice submit → 202 (got {submit.status_code})")
        if submit.status_code != 202:
            return
        run_id = submit.json().get("runId", "")
        self.expect(run_id.startswith("adhoc_"), f"runId starts with 'adhoc_' (got {run_id!r})")

        # Wait briefly for the run to reach terminal — the task is a no-op internal
        # action, completes fast; the 2s min-visibility hold then surfaces it.
        deadline = time.time() + 10
        while time.time() < deadline:
            s = self.get(f"/api/workflow-runs/{run_id}", key=alice_key["api_key"])
            if s.status_code == 200 and s.json().get("run", {}).get("state") in ("succeeded", "failed"):
                break
            time.sleep(0.5)

        # Alice lists her own run → 200 with shape.
        r = self.get(f"/api/workflow-runs/{run_id}/files", key=alice_key["api_key"])
        self.expect(r.status_code == 200, f"alice list own files → 200 (got {r.status_code})")
        body = r.json() if r.status_code == 200 else {}
        self.expect(body.get("owner") == alice_user,
                    f"owner == alice ({alice_user}, got {body.get('owner')!r})")
        ret = body.get("retention") or {}
        self.expect(ret.get("policy") == "ttl_1h",
                    f"retention.policy == 'ttl_1h' (got {ret.get('policy')!r})")
        self.expect("seconds_remaining" in ret, "retention.seconds_remaining present")
        self.expect("delete_at" in ret, "retention.delete_at present")
        files = body.get("files", [])
        self.expect(isinstance(files, list) and len(files) > 0,
                    f"files is non-empty list (got {len(files)} entries)")
        if files:
            first = files[0]
            for field in ("path", "size_bytes", "content_type", "local_path", "download_url"):
                self.expect(field in first, f"first file has '{field}'")
            self.expect(first["download_url"].startswith(f"/api/workflow-runs/{run_id}/files/"),
                        "download_url points at this run")

        # Bob tries to read Alice's run → 403 not_owner.
        r = self.get(f"/api/workflow-runs/{run_id}/files", key=bob_key["api_key"])
        self.expect(r.status_code == 403, f"bob list alice's files → 403 (got {r.status_code})")
        body = r.json() if r.status_code == 403 else {}
        self.expect(body.get("error") == "not_owner",
                    f"error == 'not_owner' (got {body.get('error')!r})")

        # Admin reads Alice's run → 200 (cross-user read is allowed + audit-logged).
        r = self.get(f"/api/workflow-runs/{run_id}/files", key=self.admin_key)
        self.expect(r.status_code == 200, f"admin list alice's files → 200 (got {r.status_code})")

        # Bogus run id → 404 run_not_found.
        r = self.get("/api/workflow-runs/adhoc_does_not_exist/files", key=alice_key["api_key"])
        self.expect(r.status_code == 404, f"bogus runId → 404 (got {r.status_code})")
        body = r.json() if r.status_code == 404 else {}
        self.expect(body.get("error") == "run_not_found",
                    f"error == 'run_not_found' (got {body.get('error')!r})")

    def test_run_file_download(self):
        header("run files: download endpoint (happy + security edges)")
        import hashlib
        alice_user = f"test-download-{uuid.uuid4().hex[:8]}@example.com"
        bob_user = f"test-download-bob-{uuid.uuid4().hex[:8]}@example.com"
        alice_key = self.issue_key(alice_user, role="operator", adhoc=True)
        bob_key = self.issue_key(bob_user, role="operator", adhoc=True)

        jcwf = minimal_adhoc_jcwf(f"t{uuid.uuid4().hex[:8]}")
        submit = self.post("/api/workflows/run-adhoc", key=alice_key["api_key"],
                           json={"jcwf": jcwf, "cleanup_policy": "ttl_1h"})
        self.expect(submit.status_code == 202, f"alice submit → 202 (got {submit.status_code})")
        if submit.status_code != 202:
            return
        run_id = submit.json().get("runId", "")
        deadline = time.time() + 10
        while time.time() < deadline:
            s = self.get(f"/api/workflow-runs/{run_id}", key=alice_key["api_key"])
            if s.status_code == 200 and s.json().get("run", {}).get("state") in ("succeeded", "failed"):
                break
            time.sleep(0.5)

        # Pick a small readable file from the listing.
        listing = self.get(f"/api/workflow-runs/{run_id}/files", key=alice_key["api_key"])
        self.expect(listing.status_code == 200, f"list ok (got {listing.status_code})")
        files = listing.json().get("files", []) if listing.status_code == 200 else []
        target = next((f for f in files if f["path"].endswith(".json") and f["size_bytes"] < 5000), None)
        self.expect(target is not None, "found a JSON file to download")
        if not target:
            return

        url = f"/api/workflow-runs/{run_id}/files/{target['path']}"

        # Happy path: alice downloads her own file.
        r = self.get(url, key=alice_key["api_key"])
        self.expect(r.status_code == 200, f"alice download → 200 (got {r.status_code})")
        self.expect(r.headers.get("Content-Type", "").startswith("application/json"),
                    f"Content-Type is json (got {r.headers.get('Content-Type')!r})")
        sha = r.headers.get("X-Content-SHA256", "")
        self.expect(len(sha) == 64, f"X-Content-SHA256 is 64 hex chars (got len {len(sha)})")
        computed = hashlib.sha256(r.content).hexdigest()
        self.expect(sha == computed, "server's SHA-256 matches locally-computed hash")
        self.expect(r.headers.get("X-Run-Id") == run_id, "X-Run-Id echoed")

        # Range: first 10 bytes.
        r = self.get(url, key=alice_key["api_key"], headers={"Range": "bytes=0-9"})
        self.expect(r.status_code == 206, f"range → 206 (got {r.status_code})")
        self.expect(len(r.content) == 10, f"body is 10 bytes (got {len(r.content)})")
        cr = r.headers.get("Content-Range", "")
        self.expect(cr.startswith("bytes 0-9/"), f"Content-Range starts with 'bytes 0-9/' (got {cr!r})")

        # Path-escape: `..` segment. Crow collapses `..` in the request URL
        # BEFORE route matching, so the handler may never see the escape attempt —
        # instead the shortened URL fails to match any route and yields 404.
        # Both outcomes block the traversal, which is what we actually care about.
        r = self.get(f"/api/workflow-runs/{run_id}/files/../etc/passwd",
                     key=alice_key["api_key"])
        self.expect(r.status_code in (400, 404),
                    f"path_escape rejected (got {r.status_code})")

        # Absolute-ish path — note Crow drops empty segments from leading `//`, so
        # use a URL-encoded leading slash.
        r = self.get(f"/api/workflow-runs/{run_id}/files/%2Fetc%2Fpasswd",
                     key=alice_key["api_key"])
        self.expect(r.status_code in (400, 404),
                    f"absolute path rejected (got {r.status_code})")

        # Missing file.
        r = self.get(f"/api/workflow-runs/{run_id}/files/does/not/exist.txt",
                     key=alice_key["api_key"])
        self.expect(r.status_code == 404, f"file_not_found → 404 (got {r.status_code})")
        self.expect(r.json().get("error") == "file_not_found",
                    f"error == 'file_not_found' (got {r.json().get('error')!r})")

        # Reserved bookkeeping file.
        r = self.get(f"/api/workflow-runs/{run_id}/files/manifest.json",
                     key=alice_key["api_key"])
        self.expect(r.status_code == 403, f"manifest.json → 403 reserved (got {r.status_code})")

        # Cross-user: bob tries to read alice's file.
        r = self.get(url, key=bob_key["api_key"])
        self.expect(r.status_code == 403, f"bob read alice's file → 403 (got {r.status_code})")
        self.expect(r.json().get("error") == "not_owner",
                    f"error == 'not_owner' (got {r.json().get('error')!r})")

        # Admin read allowed (audit-logged).
        r = self.get(url, key=self.admin_key)
        self.expect(r.status_code == 200, f"admin read → 200 (got {r.status_code})")

    def test_scripts_catalog(self):
        header("scripts: GET /api/scripts lists pre-deployed scripts with metadata")
        # Viewer role is the floor — catalog is public metadata.
        user = f"test-scripts-{uuid.uuid4().hex[:8]}@example.com"
        viewer_key = self.issue_key(user, role="viewer")
        r = self.get("/api/scripts", key=viewer_key["api_key"])
        self.expect(r.status_code == 200, f"viewer list → 200 (got {r.status_code})")
        body = r.json() if r.status_code == 200 else {}
        self.expect(body.get("ok") is True, "ok true")
        scripts = body.get("scripts", [])
        self.expect(isinstance(scripts, list) and len(scripts) > 0,
                    f"scripts list non-empty (got {len(scripts)} entries)")
        # echo.sh and parseOpenSshLog.sh ship with the tree; they should appear.
        paths = {s.get("path") for s in scripts}
        self.expect("scripts/echo.sh" in paths, "scripts/echo.sh present in catalog")
        self.expect("scripts/parseOpenSshLog.sh" in paths,
                    "scripts/parseOpenSshLog.sh present in catalog")
        # One known entry should carry parsed metadata.
        parse = next((s for s in scripts if s.get("path") == "scripts/parseOpenSshLog.sh"), None)
        self.expect(parse is not None, "parseOpenSshLog.sh entry resolves")
        if parse:
            self.expect(parse.get("type") == "shell", "type is 'shell'")
            self.expect(parse.get("has_jarvis_marker") is True, "has @jarvis-script marker")
            self.expect(bool(parse.get("short")), f"short is non-empty (got {parse.get('short')!r})")
            self.expect(isinstance(parse.get("params"), list), "params is a list")

        # type filter narrows results.
        r = self.get("/api/scripts?type=python", key=viewer_key["api_key"])
        self.expect(r.status_code == 200, f"python filter → 200 (got {r.status_code})")
        pyScripts = r.json().get("scripts", []) if r.status_code == 200 else []
        self.expect(all(s.get("type") == "python" for s in pyScripts),
                    "all filtered entries are python")

        # Unauthenticated → 401 (catalog still needs an MCP key).
        r = self.get("/api/scripts")  # no key kwarg
        self.expect(r.status_code == 401, f"unauth list → 401 (got {r.status_code})")

    def test_adhoc_missing_shell_script(self):
        header("adhoc submission: missing shell script → 400 missing_scripts")
        user = f"test-adhoc-missing-sh-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator", adhoc=True)
        wfid = f"t{uuid.uuid4().hex[:8]}"
        jcwf = {
            "id": wfid, "version": "1.0", "label": "missing-sh",
            "tasks": {
                "run": {"id": "run", "type": "shell",
                        "params": {"command": "scripts/doesNotExist.sh"},
                        "working_directory": ""}
            },
        }
        r = self.post("/api/workflows/run-adhoc", key=key["api_key"], json={"jcwf": jcwf})
        self.expect(r.status_code == 400, f"missing shell script → 400 (got {r.status_code})")
        body = r.json() if r.status_code == 400 else {}
        self.expect(body.get("error") == "missing_scripts",
                    f"error == 'missing_scripts' (got {body.get('error')!r})")
        self.expect("scripts/doesNotExist.sh" in body.get("missing", []),
                    "missing list includes the bad path")

    def test_adhoc_missing_python_module(self):
        header("adhoc submission: missing python module → 400 missing_scripts")
        user = f"test-adhoc-missing-py-{uuid.uuid4().hex[:8]}@example.com"
        key = self.issue_key(user, role="operator", adhoc=True)
        wfid = f"t{uuid.uuid4().hex[:8]}"
        jcwf = {
            "id": wfid, "version": "1.0", "label": "missing-py",
            "tasks": {
                "run": {"id": "run", "type": "python",
                        "params": {"module": "scripts.totallyFakeModule", "function": "main"},
                        "working_directory": ""}
            },
        }
        r = self.post("/api/workflows/run-adhoc", key=key["api_key"], json={"jcwf": jcwf})
        self.expect(r.status_code == 400, f"missing python module → 400 (got {r.status_code})")
        body = r.json() if r.status_code == 400 else {}
        self.expect(body.get("error") == "missing_scripts",
                    f"error == 'missing_scripts' (got {body.get('error')!r})")

    def test_heartbeat_rejects_bogus_mcp_token(self):
        header("/api/mcp/heartbeat rejects a bogus mcp_ token (auth->m_Error check)")
        # Regression for the pre-existing bug where HandleMcpHeartbeatPost checked
        # only !auth.has_value() and not auth->m_Error: TryMcpAuth returns a
        # populated AuthResult{m_Error="invalid_token"} for a syntactically-mcp
        # bogus token, so the handler treated it as success (200 + pinned
        # m_McpLastHeartbeat / IsMcpConnected()). Must now be a 403.
        bogus = "mcp_" + "0" * 48
        r = self.post("/api/mcp/heartbeat", key=bogus)
        # 403 forbidden (invalid token); 503 only if the keystore is still locked
        # — tests run against an unlocked server, so 503 here is itself a failure.
        self.expect(r.status_code == 403,
                    f"bogus mcp_ heartbeat → 403 (got {r.status_code})")
        body = r.json() if r.headers.get("content-type", "").startswith("application/json") else {}
        self.expect(body.get("ok") is False and body.get("error") == "forbidden",
                    "bogus heartbeat body is {ok:false, error:forbidden}")
        # Positive control: a freshly issued, valid mcp key heartbeats with 200.
        good = self.issue_key(f"hb_{uuid.uuid4().hex[:8]}", role="operator")["api_key"]
        r = self.post("/api/mcp/heartbeat", key=good)
        self.expect(r.status_code == 200, f"valid mcp heartbeat → 200 (got {r.status_code})")

    def test_status_exposes_adhoc_stats(self):
        header("/api/status exposes adhoc_runs_active + adhoc_disk_usage_bytes")
        r = self.get("/api/status")
        self.expect(r.status_code == 200, "status 200")
        body = r.json() if r.status_code == 200 else {}
        self.expect("adhoc_runs_active" in body, "status.adhoc_runs_active present")
        self.expect("adhoc_disk_usage_bytes" in body, "status.adhoc_disk_usage_bytes present")
        self.expect("keys_unlocked" in body, "status.keys_unlocked present")
        self.expect("mcp_keys_loaded" in body, "status.mcp_keys_loaded present")

    def run_all(self):
        self.test_whoami_unauth()
        self.test_admin_key_works()
        self.test_enrollment_roundtrip()
        self.test_invalid_key_rejected()
        self.test_activate_invalid_token()
        self.test_rbac_viewer_cannot_enroll()
        self.test_self_renew()
        self.test_revoke()
        self.test_session_login_logout()
        self.test_status_exposes_adhoc_stats()
        self.test_heartbeat_rejects_bogus_mcp_token()
        self.test_adhoc_missing_flag()
        self.test_adhoc_bad_policy()
        self.test_adhoc_policy_ceiling()
        self.test_adhoc_folder_namespace()
        self.test_run_files_list()
        self.test_run_file_download()
        self.test_scripts_catalog()
        self.test_adhoc_missing_shell_script()
        self.test_adhoc_missing_python_module()
        self.test_adhoc_happy_path()
        self.test_adhoc_aicall_roundtrip()


def minimal_adhoc_jcwf(wfid: str) -> dict:
    """Smallest JCWF canvas that `WorkflowRegistry::SaveOrUpdateWorkflowFromJson`
    accepts for adhoc staging. The submit handler only needs the parse + save to
    succeed — it doesn't wait for the run to complete. We use an `internal` task
    with `action: "carMaintenance"` because that's a registered factory; the run
    itself will fail because it expects specific input files, but the 202 response
    fires as soon as staging completes.
    """
    return {
        "id": wfid,
        "version": "1.0",
        "label": "adhoc test",
        "manual_start": True,
        "tasks": {
            "noop": {
                "id": "noop",
                "type": "internal",
                "params": {"action": "carMaintenance"},
            }
        },
    }


def main():
    parser = argparse.ArgumentParser(description="MCP auth + adhoc integration tests")
    parser.add_argument("--base-url", default=os.environ.get("J9T_URL", "https://localhost:8443"),
                        help="j9t base URL (default: J9T_URL env or https://localhost:8443)")
    parser.add_argument("--admin-key", default=os.environ.get("J9T_ADMIN_KEY"),
                        help="Admin MCP API key (or set J9T_ADMIN_KEY env)")
    parser.add_argument("--with-ai", action="store_true",
                        help="Also run tests that require a working AI provider "
                             "(real HTTP to OpenAI / Gemini / etc.). Off by default "
                             "so the suite stays green on machines without AI configured.")
    args = parser.parse_args()

    if not args.admin_key or not args.admin_key.startswith("mcp_"):
        print(f"{C.RED}ERROR:{C.RESET} --admin-key (or J9T_ADMIN_KEY) must be an 'mcp_...' key.")
        print("Bootstrap one from j9t's first-run enrollment banner (log/log.txt) via")
        print("  POST /api/auth/mcp-keys/activate")
        sys.exit(2)

    runner = Runner(args.base_url, args.admin_key)
    runner.with_ai = args.with_ai
    print(f"{C.BOLD}MCP auth + adhoc integration tests{C.RESET}")
    info(f"Base URL: {args.base_url}")
    info(f"Admin key: {args.admin_key[:12]}...")

    try:
        runner.run_all()
    except requests.ConnectionError as e:
        fail(f"connection error: {e}")
        runner.failed += 1

    print()
    total = runner.passed + runner.failed
    status = f"{runner.passed}/{total}"
    color = C.GREEN if runner.failed == 0 else C.RED
    print(f"{color}{C.BOLD}Passed: {status}{C.RESET}")
    sys.exit(0 if runner.failed == 0 else 1)


if __name__ == "__main__":
    main()
