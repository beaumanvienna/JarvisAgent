#!/usr/bin/env python3
"""
Workflow-folder files endpoints — list + upload.

Verifies GET/POST /api/workflows/<id>/files (added for the editor's
artifact-file nodes / drag-drop upload).  Each assertion is REST-observable
against a *live* j9t (default https://localhost:8443); the host instance must
be running with the keystore unlocked and an admin MCP key in $J9T_TOKEN.

The test creates a throwaway workflow, exercises the endpoints, and deletes it
in a cleanup pass — no pre-existing workflow is mutated.

Coverage:

  Setup    create a temp workflow (POST /api/workflows) so workflows/<id>/ exists

  Happy    G1  GET  …/files        → 200, lists global.json + <id>.json canvas
           G2  POST …/files        → 201 {path, size_bytes}; multipart field "file"
           G3  GET  …/files        → the uploaded file now appears

  Security S1  traversal filename "../escape.csv" → 201, stored as basename
                 "escape.csv" (path-stripped, never escapes the folder)
           S2  25 MB cap: 26 MB upload → 413 file_too_large, not written
           S3  no auth                → 401
           S4  wrong Content-Type     → 400 invalid_content_type
           S5  missing workflow       → 404 workflow_not_found
           S6  invalid workflow id    → 400 invalid_workflow_id
           S7  auth split: operator may GET (200) but POST upload → 403
           C1  GET …/files/<path> reads content; C2 traversal blocked (no leak);
                 C3 missing → 404; C4 no auth → 401

  Cleanup  DELETE the temp workflow; revoke the operator key

Usage:

  python3 test/hardening/test_workflow_files_endpoint.py
  python3 test/hardening/test_workflow_files_endpoint.py --admin-key "$J9T_TOKEN"
"""

import argparse
import os
import sys
import uuid

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
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'─'*70}\n  {msg}\n{'─'*70}{C.RESET}")


def http(method, base, path, key=None, **kw):
    headers = kw.pop("headers", {})
    if key:
        headers["Authorization"] = f"Bearer {key}"
    verify_ssl = not base.startswith("https://localhost")
    return requests.request(method, f"{base.rstrip('/')}{path}",
                            timeout=30, headers=headers, verify=verify_ssl, **kw)


def expect(cond, msg, results):
    if cond:
        ok(msg); results[0] += 1
    else:
        fail(msg); results[1] += 1
    return cond


def expect_status_ok(base, admin_key, results):
    r = http("GET", base, "/api/status", key=admin_key)
    expect(r.status_code == 200, f"engine still alive (/api/status → {r.status_code})", results)


def issue_operator_key(base, admin_key, user):
    enroll = http("POST", base, "/api/auth/mcp-keys/enroll", key=admin_key, json={
        "user": user, "role": "operator", "adhoc_enabled": False,
        "disk_quota_mb": 256, "default_cleanup_policy": "ttl_72h",
        "description": "files-endpoint test key", "key_expiry_days": 1,
        "enrollment_ttl_minutes": 5,
    })
    if enroll.status_code != 201:
        raise RuntimeError(f"enroll failed: {enroll.status_code} {enroll.text}")
    activate = http("POST", base, "/api/auth/mcp-keys/activate",
                    json={"enrollment_token": enroll.json()["enrollment_token"]})
    if activate.status_code != 200:
        raise RuntimeError(f"activate failed: {activate.status_code} {activate.text}")
    return activate.json()


def minimal_jcwf(wfid):
    return {
        "id": wfid, "version": "1.0", "label": "files-endpoint test",
        "manual_start": True,
        "tasks": {"noop": {"id": "noop", "type": "internal",
                            "params": {"action": "carMaintenance"}}},
    }


def main():
    parser = argparse.ArgumentParser(description="Workflow-folder files endpoints test")
    parser.add_argument("--base", default="https://localhost:8443")
    parser.add_argument("--admin-key", default=os.environ.get("J9T_TOKEN", ""))
    args = parser.parse_args()

    if not args.admin_key:
        print("ERROR: provide --admin-key or set $J9T_TOKEN")
        sys.exit(2)

    base, admin = args.base, args.admin_key
    results = [0, 0]  # [passed, failed]

    wfid = f"t{uuid.uuid4().hex[:10]}"
    files_path = f"/api/workflows/{wfid}/files"
    op_key = None

    # Preflight: confirm the keystore is unlocked / admin key valid.
    who = http("GET", base, "/api/auth/whoami", key=admin)
    if who.status_code != 200:
        print(f"{C.RED}Preflight failed:{C.RESET} /api/auth/whoami → {who.status_code} {who.text}")
        print("Is j9t running with the keystore unlocked and $J9T_TOKEN an admin key?")
        sys.exit(2)

    try:
        header(f"Setup — create temp workflow '{wfid}'")
        r = http("POST", base, "/api/workflows", key=admin, json=minimal_jcwf(wfid))
        if not expect(r.status_code == 201, f"create temp workflow → 201 (got {r.status_code})", results):
            print(f"{C.RED}Cannot proceed without a temp workflow: {r.text}{C.RESET}")
            sys.exit(1)

        header("G1  GET …/files lists the workflow folder")
        r = http("GET", base, files_path, key=admin)
        expect(r.status_code == 200, f"GET files → 200 (got {r.status_code})", results)
        body = r.json() if r.headers.get("content-type", "").startswith("application/json") else {}
        paths = [f["path"] for f in body.get("files", [])]
        expect(body.get("ok") is True, "response ok:true", results)
        expect(any(p.endswith("global.json") for p in paths), "lists global.json", results)

        header("G2  POST …/files uploads a file")
        r = http("POST", base, files_path, key=admin,
                 files={"file": ("port62pos.csv", b"symbol,qty\nAAPL,10\n", "text/csv")})
        expect(r.status_code == 201, f"upload → 201 (got {r.status_code})", results)
        ub = r.json()
        expect(ub.get("path") == "port62pos.csv", f"returned path == port62pos.csv (got {ub.get('path')})", results)
        expect(ub.get("size_bytes") == 19, f"size_bytes == 19 (got {ub.get('size_bytes')})", results)

        header("G3  GET …/files shows the uploaded file")
        r = http("GET", base, files_path, key=admin)
        paths = [f["path"] for f in r.json().get("files", [])]
        expect("port62pos.csv" in paths, "uploaded file appears in listing", results)

        header("S1  traversal filename is stripped to its basename")
        r = http("POST", base, files_path, key=admin,
                 files={"file": ("../escape.csv", b"x,y\n1,2\n", "text/csv")})
        expect(r.status_code == 201, f"traversal upload → 201 (got {r.status_code})", results)
        expect(r.json().get("path") == "escape.csv",
               f"stored as basename 'escape.csv' (got {r.json().get('path')})", results)
        r = http("GET", base, files_path, key=admin)
        paths = [f["path"] for f in r.json().get("files", [])]
        expect("escape.csv" in paths and not any(".." in p for p in paths),
               "listing has 'escape.csv' and no '..' path (no escape)", results)

        header("S2  25 MB cap rejects a 26 MB upload")
        big = b"x" * (26 * 1024 * 1024)
        r = http("POST", base, files_path, key=admin,
                 files={"file": ("big.bin", big, "application/octet-stream")})
        expect(r.status_code == 413, f"26 MB upload → 413 (got {r.status_code})", results)
        expect(r.json().get("error") == "file_too_large", "error == file_too_large", results)
        r = http("GET", base, files_path, key=admin)
        paths = [f["path"] for f in r.json().get("files", [])]
        expect("big.bin" not in paths, "oversized file not written", results)

        header("S3  upload without auth → 401")
        r = http("POST", base, files_path,
                 files={"file": ("x.csv", b"a\n", "text/csv")})
        expect(r.status_code == 401, f"no auth → 401 (got {r.status_code})", results)

        header("S4  wrong Content-Type → 400 invalid_content_type")
        r = http("POST", base, files_path, key=admin, json={"not": "multipart"})
        expect(r.status_code == 400, f"json body → 400 (got {r.status_code})", results)
        expect(r.json().get("error") == "invalid_content_type", "error == invalid_content_type", results)

        header("S5  upload to a missing workflow → 404")
        r = http("POST", base, "/api/workflows/no_such_wf_xyz/files", key=admin,
                 files={"file": ("x.csv", b"a\n", "text/csv")})
        expect(r.status_code == 404, f"missing workflow → 404 (got {r.status_code})", results)
        expect(r.json().get("error") == "workflow_not_found", "error == workflow_not_found", results)

        header("S6  invalid workflow id → 400")
        r = http("GET", base, "/api/workflows/bad%2Fid/files", key=admin)
        expect(r.status_code == 400, f"id with slash → 400 (got {r.status_code})", results)

        header("S7  auth split — operator can list but not upload")
        op_user = f"files-op-{uuid.uuid4().hex[:8]}@example.com"
        op = issue_operator_key(base, admin, op_user)
        op_key = op
        r = http("GET", base, files_path, key=op["api_key"])
        expect(r.status_code == 200, f"operator GET files → 200 (got {r.status_code})", results)
        r = http("POST", base, files_path, key=op["api_key"],
                 files={"file": ("x.csv", b"a\n", "text/csv")})
        expect(r.status_code == 403, f"operator POST upload → 403 (got {r.status_code})", results)

        header("C1  GET …/files/<path> reads file content")
        r = http("GET", base, f"{files_path}/port62pos.csv", key=admin)
        expect(r.status_code == 200, f"read uploaded file → 200 (got {r.status_code})", results)
        expect("AAPL" in r.text, "content contains the uploaded data", results)

        header("C2  content read — traversal blocked, no leak")
        r = http("GET", base, f"/api/workflows/{wfid}/files/../../../etc/passwd", key=admin)
        expect(r.status_code in (400, 404), f"traversal → 400/404 (got {r.status_code})", results)
        expect("root:" not in r.text, "no /etc/passwd leak", results)

        header("C3  content read — missing file → 404")
        r = http("GET", base, f"{files_path}/nope.csv", key=admin)
        expect(r.status_code == 404, f"missing → 404 (got {r.status_code})", results)

        header("C4  content read — no auth → 401")
        r = http("GET", base, f"{files_path}/port62pos.csv")
        expect(r.status_code == 401, f"no auth → 401 (got {r.status_code})", results)

        expect_status_ok(base, admin, results)

    finally:
        header("Cleanup")
        d = http("DELETE", base, f"/api/workflows/{wfid}", key=admin)
        info(f"deleted temp workflow '{wfid}' → {d.status_code}")
        if op_key is not None:
            http("DELETE", base, f"/api/auth/mcp-keys/{op_key.get('key_id', '')}", key=admin)
            info("revoked operator key")

    passed, failed = results
    print(f"\n{C.BOLD}{'═'*70}{C.RESET}")
    print(f"  {C.GREEN}{passed} passed{C.RESET}, "
          f"{C.RED if failed else C.GREEN}{failed} failed{C.RESET}")
    print(f"{C.BOLD}{'═'*70}{C.RESET}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
