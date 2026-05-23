#!/usr/bin/env python3
"""
Collision-repro test for AdhocWorkflowManager::SanitizeUserSlug.

Provisions two MCP keys whose users collapse to the same character-cleaned
slug pre-fix (e.g. `bob+admin@example.com` and `bob_admin@example.com` both
sanitise to `bob_admin@example.com`).  Verifies:

  1. Each user's adhoc run lands under a DISTINCT `_adhoc/<user_slug>/` dir
     after the SHA-256 suffix swap — the published `owner_slug` ends with
     `_<8 hex chars>` and the two hex suffixes differ.
  2. The body characters that DO collide are still the same — so the suffix
     is genuinely what separates them.
  3. Cross-user reads return 403 not_owner (the authz primitive switched to
     m_User, not the slug, so collision-prone slugs no longer leak access).
  4. Admin cross-user read returns 200 (and the security log will carry an
     INFO `admin_cross_user_read` audit line — verify by tailing log/log.txt
     after the run).

Runs against a live JarvisAgent instance.  Requires an MCP admin key.

  python3 test/security/test_adhoc_user_slug_collision.py \\
      --admin-key "$J9T_TOKEN"

The default base URL is https://localhost:8443 (matches j9t's production
listen port configured in config.json).
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
    print("ERROR: 'requests' package missing (pip install requests)")
    sys.exit(1)


class C:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    RED = "\033[91m"
    GREEN = "\033[92m"
    CYAN = "\033[96m"


def ok(msg):     print(f"  {C.GREEN}✓{C.RESET} {msg}")
def fail(msg):   print(f"  {C.RED}✗{C.RESET} {msg}")
def info(msg):   print(f"  {C.CYAN}ℹ{C.RESET} {msg}")
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'─'*60}\n  {msg}\n{'─'*60}{C.RESET}")


def minimal_adhoc_jcwf(wfid: str) -> dict:
    return {
        "id": wfid,
        "version": "1.0",
        "label": "slug collision test",
        "manual_start": True,
        "tasks": {
            "noop": {
                "id": "noop",
                "type": "internal",
                "params": {"action": "carMaintenance"},
            }
        },
    }


def expect(cond, msg, results):
    if cond:
        ok(msg)
        results[0] += 1
    else:
        fail(msg)
        results[1] += 1
    return cond


def http(method, base, path, key=None, **kw):
    headers = kw.pop("headers", {})
    if key:
        headers["Authorization"] = f"Bearer {key}"
    verify_ssl = not base.startswith("https://localhost")
    return requests.request(method, f"{base.rstrip('/')}{path}",
                            timeout=15, headers=headers, verify=verify_ssl, **kw)


def issue_key(base, admin_key, user):
    enroll = http("POST", base, "/api/auth/mcp-keys/enroll", key=admin_key, json={
        "user": user,
        "role": "operator",
        "adhoc_enabled": True,
        "disk_quota_mb": 1024,
        "default_cleanup_policy": "ttl_72h",
        "description": "slug collision test",
        "key_expiry_days": 1,
        "enrollment_ttl_minutes": 5,
    })
    if enroll.status_code != 201:
        raise RuntimeError(f"enroll '{user}' failed: {enroll.status_code} {enroll.text}")
    activate = http("POST", base, "/api/auth/mcp-keys/activate",
                    json={"enrollment_token": enroll.json()["enrollment_token"]})
    if activate.status_code != 200:
        raise RuntimeError(f"activate '{user}' failed: {activate.status_code} {activate.text}")
    return activate.json()


def revoke_key(base, admin_key, key_id):
    http("DELETE", base, f"/api/auth/mcp-keys/{key_id}", key=admin_key)


def stage_adhoc(base, user_key, wfid):
    jcwf = minimal_adhoc_jcwf(wfid)
    r = http("POST", base, "/api/workflows/run-adhoc", key=user_key,
             json={"jcwf": jcwf, "cleanup_policy": "ttl_1h"})
    if r.status_code != 202:
        raise RuntimeError(f"stage run failed: {r.status_code} {r.text[:300]}")
    return r.json()["runId"]


def get_owner_slug(base, key, run_id):
    """Drive the file-list endpoint and read body['owner_slug']."""
    deadline = time.time() + 8
    while time.time() < deadline:
        r = http("GET", base, f"/api/workflow-runs/{run_id}/files", key=key)
        if r.status_code == 200:
            return r.json().get("owner_slug", ""), r.json()
        time.sleep(0.2)
    raise RuntimeError(f"file list never became readable for {run_id}: HTTP {r.status_code}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=os.environ.get("J9T_URL", "https://localhost:8443"))
    parser.add_argument("--admin-key",
                        default=os.environ.get("J9T_TOKEN") or os.environ.get("J9T_ADMIN_KEY"))
    args = parser.parse_args()

    if not args.admin_key or not args.admin_key.startswith("mcp_"):
        print(f"{C.RED}ERROR:{C.RESET} --admin-key (or J9T_TOKEN / J9T_ADMIN_KEY) must be an 'mcp_...' key.")
        return 2

    # Crafted users whose SanitizeUserSlug body (`[A-Za-z0-9._@-]`, others → `_`)
    # collapses to the SAME string: both produce `bob_admin@example.com`.  The
    # SHA-256 suffix is what now keeps them apart.  Suffix `-<uuid>` ensures
    # repeated runs don't pile up.
    nonce = uuid.uuid4().hex[:8]
    user_plus  = f"bob+admin-{nonce}@example.com"
    user_under = f"bob_admin-{nonce}@example.com"

    results = [0, 0]  # [passed, failed]

    info(f"base = {args.base_url}")
    info(f"user_plus  = {user_plus}")
    info(f"user_under = {user_under}")

    key_plus = None
    key_under = None
    try:
        header("Provision two MCP keys with pre-fix-colliding users")
        key_plus = issue_key(args.base_url, args.admin_key, user_plus)
        key_under = issue_key(args.base_url, args.admin_key, user_under)
        expect(key_plus["api_key"].startswith("mcp_"), "plus-user key issued", results)
        expect(key_under["api_key"].startswith("mcp_"), "under-user key issued", results)

        header("Stage one adhoc run per user")
        run_plus = stage_adhoc(args.base_url, key_plus["api_key"], f"t{nonce}p")
        run_under = stage_adhoc(args.base_url, key_under["api_key"], f"t{nonce}u")
        info(f"run_plus  = {run_plus}")
        info(f"run_under = {run_under}")

        header("Owner slugs are distinct (the bug fix)")
        slug_plus, list_plus = get_owner_slug(args.base_url, key_plus["api_key"], run_plus)
        slug_under, list_under = get_owner_slug(args.base_url, key_under["api_key"], run_under)
        info(f"slug_plus  = {slug_plus}")
        info(f"slug_under = {slug_under}")

        expect(slug_plus != slug_under,
               f"distinct owner_slugs ({slug_plus!r} != {slug_under!r}) — collision avoided", results)

        # Each slug ends with `_<8 hex>` per the new SanitizeUserSlug shape.
        expect(len(slug_plus) >= 9 and slug_plus[-9] == '_' and all(c in "0123456789abcdef" for c in slug_plus[-8:]),
               f"slug_plus ends with `_<8hex>` (got {slug_plus[-9:]!r})", results)
        expect(len(slug_under) >= 9 and slug_under[-9] == '_' and all(c in "0123456789abcdef" for c in slug_under[-8:]),
               f"slug_under ends with `_<8hex>` (got {slug_under[-9:]!r})", results)

        # Pre-fix the bodies would have been IDENTICAL.  Now they're distinct
        # only because of the hash suffix — verify by chopping the suffix off.
        body_plus = slug_plus[:-9]
        body_under = slug_under[:-9]
        expect(body_plus == body_under,
               f"slug BODIES still collide pre-suffix ({body_plus!r}) — proves the suffix is doing the work",
               results)
        expect(slug_plus[-8:] != slug_under[-8:],
               f"hash suffixes differ ({slug_plus[-8:]!r} vs {slug_under[-8:]!r})", results)

        header("Cross-user access denied (authz on m_User, not slug)")
        # plus-user tries to read under-user's files → 403 not_owner.
        r = http("GET", args.base_url, f"/api/workflow-runs/{run_under}/files",
                 key=key_plus["api_key"])
        expect(r.status_code == 403,
               f"plus → under's files = 403 (got {r.status_code})", results)
        if r.status_code == 403:
            expect(r.json().get("error") == "not_owner",
                   f"error == 'not_owner' (got {r.json().get('error')!r})", results)

        # under-user tries to read plus-user's files → 403 not_owner.
        r = http("GET", args.base_url, f"/api/workflow-runs/{run_plus}/files",
                 key=key_under["api_key"])
        expect(r.status_code == 403,
               f"under → plus's files = 403 (got {r.status_code})", results)

        header("Admin cross-user read allowed (+ audit log)")
        r = http("GET", args.base_url, f"/api/workflow-runs/{run_plus}/files",
                 key=args.admin_key)
        expect(r.status_code == 200,
               f"admin → plus's files = 200 (got {r.status_code})", results)
        r = http("GET", args.base_url, f"/api/workflow-runs/{run_under}/files",
                 key=args.admin_key)
        expect(r.status_code == 200,
               f"admin → under's files = 200 (got {r.status_code})", results)
        info("Look for `[security] admin_cross_user_read kind=list caller=<admin>` in log/log.txt")

        header("Self-access still works (sanity)")
        r = http("GET", args.base_url, f"/api/workflow-runs/{run_plus}/files",
                 key=key_plus["api_key"])
        expect(r.status_code == 200,
               f"plus → plus's own files = 200 (got {r.status_code})", results)
        r = http("GET", args.base_url, f"/api/workflow-runs/{run_under}/files",
                 key=key_under["api_key"])
        expect(r.status_code == 200,
               f"under → under's own files = 200 (got {r.status_code})", results)

    finally:
        # Best-effort revoke; if the server is already gone we don't care.
        try:
            if key_plus: revoke_key(args.base_url, args.admin_key, key_plus["key_id"])
            if key_under: revoke_key(args.base_url, args.admin_key, key_under["key_id"])
        except Exception as e:
            info(f"cleanup: revoke failed: {e}")

    passed, failed = results
    print()
    if failed == 0:
        print(f"{C.GREEN}PASS:{C.RESET} {passed} checks")
        return 0
    print(f"{C.RED}FAIL:{C.RESET} {passed} passed, {failed} failed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
