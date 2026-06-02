#!/usr/bin/env python3
"""
Live S3 round-trip smoke for the consolidated SigV4 signer (Sitting 16 basket #12).

After the two parallel SigV4Signer classes were collapsed into the single engine
signer (basket #1), the heap-scan audit + Bedrock KAT cover residue + canonical-
request correctness, but no LIVE S3 request had exercised the engine signer's S3
dispatch shape — in particular the new `m_ExtraHeadersToSign` path that folds
`Content-Type` into the canonical headers on a PUT.

This drives a full upload → download → delete round-trip against the configured
`my-s3` connection (minio on JC's host) via a single adhoc JCWF:

  mk  (python)  writes a 65538-byte canary (scripts/writeOutputCapCanary.py)
  up  (s3)      upload  — PUT, Content-Type signed via m_ExtraHeadersToSign
  dl  (s3)      download — GET, signed
  del (s3)      delete  — DELETE, signed

All four tasks share one working_directory so the canary written by `mk` is the
file `up` uploads.  A `succeeded` terminal state means every signed request was
accepted by the endpoint — i.e. the consolidated signer produces wire-correct
signatures for all three S3 verbs live.

Prereqs: j9t running + keystore unlocked + `my-s3` connection online (minio up).

Usage:
  python3 test/dispatch/test_s3_roundtrip.py --admin-key "$J9T_TOKEN"
  python3 test/dispatch/test_s3_roundtrip.py --admin-key "$J9T_TOKEN" --connection my-s3
"""

import argparse
import os
import sys
import time
import uuid

try:
    import requests
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
except ImportError:
    print("ERROR: 'requests' not installed (pip install requests)")
    sys.exit(1)


class C:
    RESET = "\033[0m"; BOLD = "\033[1m"
    RED = "\033[91m"; GREEN = "\033[92m"; CYAN = "\033[96m"; YELLOW = "\033[93m"


def ok(m):   print(f"  {C.GREEN}✓{C.RESET} {m}")
def fail(m): print(f"  {C.RED}✗{C.RESET} {m}")
def info(m): print(f"  {C.CYAN}ℹ{C.RESET} {m}")
def warn(m): print(f"  {C.YELLOW}⚠{C.RESET} {m}")
def header(m): print(f"\n{C.BOLD}{C.CYAN}{'─'*70}\n  {m}\n{'─'*70}{C.RESET}")


def http(method, base, path, key=None, **kw):
    headers = kw.pop("headers", {})
    if key:
        headers["Authorization"] = f"Bearer {key}"
    verify = not base.startswith("https://localhost")
    return requests.request(method, f"{base.rstrip('/')}{path}", timeout=30,
                            headers=headers, verify=verify, **kw)


def issue_adhoc_key(base, admin_key, user):
    enroll = {"user": user, "role": "operator", "adhoc_enabled": True,
              "disk_quota_mb": 1024, "default_cleanup_policy": "ttl_72h",
              "description": "s3 roundtrip smoke", "key_expiry_days": 90,
              "enrollment_ttl_minutes": 5}
    r = http("POST", base, "/api/auth/mcp-keys/enroll", key=admin_key, json=enroll)
    if r.status_code != 201:
        raise RuntimeError(f"enroll failed: {r.status_code} {r.text}")
    token = r.json()["enrollment_token"]
    r = http("POST", base, "/api/auth/mcp-keys/activate", json={"enrollment_token": token})
    if r.status_code != 200:
        raise RuntimeError(f"activate failed: {r.status_code} {r.text}")
    return r.json()


def revoke_key(base, admin_key, key_id):
    try:
        http("DELETE", base, f"/api/auth/mcp-keys/{key_id}", key=admin_key)
    except Exception:
        pass


def poll_terminal(base, key, run_id, deadline_s=90):
    deadline = time.time() + deadline_s
    last = None
    while time.time() < deadline:
        r = http("GET", base, f"/api/workflow-runs/{run_id}", key=key)
        if r.status_code == 200:
            run = r.json().get("run", {})
            state = run.get("state")
            last = run
            if state in ("succeeded", "failed"):
                return state, run
        time.sleep(0.7)
    return None, last


def s3_roundtrip_jcwf(wfid, connection, key_name):
    shared = f"{wfid}/shared"
    return {
        "id": wfid, "version": "1.0", "label": f"s3 roundtrip smoke {wfid}",
        "manual_start": True,
        "tasks": {
            "mk": {
                "id": "mk", "type": "python", "working_directory": shared,
                "params": {"module": "writeOutputCapCanary", "function": "write_canary"},
            },
            "up": {
                "id": "up", "type": "s3", "working_directory": shared,
                "depends_on": ["mk"],
                "params": {"connection": connection, "operation": "upload",
                           "key": key_name, "file_path": "big.txt"},
            },
            "dl": {
                "id": "dl", "type": "s3", "working_directory": shared,
                "depends_on": ["up"],
                "params": {"connection": connection, "operation": "download",
                           "key": key_name, "file_path": "dl.bin"},
            },
            "del": {
                "id": "del", "type": "s3", "working_directory": shared,
                "depends_on": ["dl"],
                "params": {"connection": connection, "operation": "delete",
                           "key": key_name},
            },
        },
    }


def task_states(run):
    """Return {taskId: (state, error_or_stdout)} from the run JSON.  The run JSON
    keys each task object by 'taskId'; failures carry 'error', successes carry the
    op result in 'capturedStdout'."""
    out = {}
    for t in run.get("tasks", []) if isinstance(run.get("tasks"), list) else []:
        out[t.get("taskId")] = (t.get("state"), t.get("error") or t.get("capturedStdout", ""))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--admin-key", default=os.environ.get("J9T_TOKEN"), required=False)
    p.add_argument("--base-url", default=os.environ.get("J9T_URL", "https://localhost:8443"))
    p.add_argument("--connection", default="my-s3")
    args = p.parse_args()
    if not args.admin_key:
        print("ERROR: pass --admin-key or export J9T_TOKEN")
        return 2

    base, admin = args.base_url, args.admin_key
    passed = failed = 0

    header(f"S3 round-trip smoke against connection '{args.connection}' (consolidated signer)")
    # Connection test first — exercises the signer's GET/list path live.
    r = http("POST", base, f"/api/connections/{args.connection}/test", key=admin)
    if r.status_code == 200 and r.json().get("ok") is True:
        ok(f"connection test → ok (GET/list signing live)"); passed += 1
    else:
        fail(f"connection test → {r.status_code} {r.text[:200]} — is minio up + {args.connection} configured?")
        return 1

    key = issue_adhoc_key(base, admin, f"s3smoke_{uuid.uuid4().hex[:8]}@example.com")
    obj_key = f"j9t-s3-smoke/{uuid.uuid4().hex}.bin"
    wfid = f"s3rt_{uuid.uuid4().hex[:8]}"
    try:
        info(f"object key: {obj_key}")
        jcwf = s3_roundtrip_jcwf(wfid, args.connection, obj_key)
        r = http("POST", base, "/api/workflows/run-adhoc", key=key["api_key"],
                 json={"jcwf": jcwf, "cleanup_policy": "ttl_1h"})
        if r.status_code != 202:
            fail(f"adhoc submit → {r.status_code} {r.text[:200]}"); failed += 1
            return 1
        ok("adhoc submit → 202"); passed += 1
        run_id = r.json().get("runId")

        state, run = poll_terminal(base, key["api_key"], run_id, deadline_s=90)
        ts = task_states(run or {})
        info(f"run state = {state!r}; tasks = {ts}")
        if state == "succeeded":
            ok("run terminal = succeeded (upload PUT + download GET + delete DELETE all signed-and-accepted)")
            passed += 1
        else:
            fail(f"run terminal = {state!r} (expected succeeded)")
            failed += 1
            # Surface the first failing task to localise a signing regression.
            for tid in ("up", "dl", "del"):
                st, err = ts.get(tid, ("?", ""))
                if st != "succeeded":
                    info(f"  task '{tid}' = {st!r} error={err[:200]!r}")

        # Per-verb confirmation: each s3 task succeeded individually.
        for tid, verb in (("up", "upload/PUT"), ("dl", "download/GET"), ("del", "delete/DELETE")):
            st, err = ts.get(tid, ("missing", ""))
            if st == "succeeded":
                ok(f"s3 {verb} task succeeded"); passed += 1
            else:
                fail(f"s3 {verb} task = {st!r} error={err[:160]!r}"); failed += 1
    finally:
        revoke_key(base, admin, key["key_id"])

    print()
    if failed == 0:
        print(f"{C.GREEN}PASS:{C.RESET} {passed} checks")
        return 0
    print(f"{C.RED}FAIL:{C.RESET} {passed} passed, {failed} failed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
