#!/usr/bin/env python3
"""
End-to-end test for the AI-interface URL policy gate (Sitting 15).

The policy: plain http:// is loopback-only AND never with a key_name.  The
reference implementation lives in `application/network/urlPolicy.h` and is
wired in at:

  - `engine/json/configParser.cpp::ParseInterfaces` (config-load enforcement)
  - `application/web/webServer.cpp::HandleAiInterfaceCreatePost`
    + `HandleAiInterfaceUpdatePut` (REST enforcement)

This test drives the REST surface against a running JarvisAgent and verifies:

  1. http://example.com/v1/...           → HTTP 400 + url_policy_violation
  2. http://192.168.1.5/v1/...           → HTTP 400 + url_policy_violation
  3. http://localhost:11434/...          → HTTP 201 (loopback, no key)
  4. http://localhost:11434/... + key    → HTTP 400 + credentialed_plaintext_http
  5. https://api.openai.com/... + key    → HTTP 201 (regression check)
  6. http://[::1]:11434/...               → HTTP 201 (IPv6 loopback)
  7. http://127.0.0.1:11434/...          → HTTP 201 (literal IPv4 loopback)
  8. ws://localhost/...                  → HTTP 400 + url_policy_violation
  9. file:///etc/passwd                  → HTTP 400 + url_policy_violation
 10. empty URL                            → HTTP 400 (missing_url precedes us)
 11. /api/debug/signals counters increment with each rejection (debug builds)

Each test creates a uniquely-named interface, asserts the outcome, then
deletes any interface that did get created so the run is hermetic.

  python3 test/security/test_url_policy.py --admin-key "$J9T_TOKEN"

The default base URL is https://localhost:8443.
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
    RESET = "\033[0m"
    BOLD = "\033[1m"
    RED = "\033[91m"
    GREEN = "\033[92m"
    CYAN = "\033[96m"
    YELLOW = "\033[93m"


def ok(msg):     print(f"  {C.GREEN}✓{C.RESET} {msg}")
def fail(msg):   print(f"  {C.RED}✗{C.RESET} {msg}")
def info(msg):   print(f"  {C.CYAN}ℹ{C.RESET} {msg}")
def warn(msg):   print(f"  {C.YELLOW}⚠{C.RESET} {msg}")
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'-'*60}\n  {msg}\n{'-'*60}{C.RESET}")


def http(method, base, path, key=None, **kw):
    headers = kw.pop("headers", {})
    if key:
        headers["Authorization"] = f"Bearer {key}"
    verify_ssl = not base.startswith("https://localhost")
    return requests.request(method, f"{base.rstrip('/')}{path}",
                            timeout=15, headers=headers, verify=verify_ssl, **kw)


def expect(cond, msg, results):
    if cond:
        ok(msg)
        results[0] += 1
    else:
        fail(msg)
        results[1] += 1
    return cond


def get_signal(base, admin_key, name):
    """Returns the named signal counter, or None if /api/debug/signals is
    unavailable (release builds don't compile the endpoint)."""
    r = http("GET", base, "/api/debug/signals", key=admin_key)
    if r.status_code != 200:
        return None
    return r.json().get("signals", {}).get(name)


def create_interface(base, admin_key, name, url, key_name="", api_type="API1"):
    body = {
        "name": name,
        "url": url,
        "model": "gpt-test",
        "api_type": api_type,
    }
    if key_name:
        body["key_name"] = key_name
    return http("POST", base, "/api/settings/ai-interfaces", key=admin_key, json=body)


def delete_interface(base, admin_key, name):
    http("DELETE", base, f"/api/settings/ai-interfaces/{name}", key=admin_key)


def assert_reject(base, admin_key, name, url, key_name, expected_error_code, results):
    r = create_interface(base, admin_key, name, url, key_name)
    payload_label = f"url={url!r}" + (f" key_name={key_name!r}" if key_name else "")
    if not expect(r.status_code == 400, f"{payload_label} → HTTP 400 (got {r.status_code})", results):
        # Clean up any interface that snuck through despite a non-400.
        if r.status_code == 201:
            delete_interface(base, admin_key, name)
        return
    body = r.json()
    expect(body.get("error") == expected_error_code,
           f"  error={body.get('error')!r} (expected {expected_error_code!r})",
           results)
    if not body.get("message"):
        warn(f"  message field empty — operator-facing detail is missing")


def assert_accept(base, admin_key, name, url, key_name, results):
    r = create_interface(base, admin_key, name, url, key_name)
    payload_label = f"url={url!r}" + (f" key_name={key_name!r}" if key_name else "")
    if not expect(r.status_code == 201, f"{payload_label} → HTTP 201 (got {r.status_code} body={r.text[:200]})", results):
        return
    # Hermetic cleanup.
    delete_interface(base, admin_key, name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base-url", default=os.environ.get("J9T_URL", "https://localhost:8443"))
    parser.add_argument("--admin-key", default=os.environ.get("J9T_TOKEN"),
                        help="Admin MCP key (Bearer).  Required.")
    args = parser.parse_args()

    if not args.admin_key:
        print(f"ERROR: --admin-key or $J9T_TOKEN required.")
        return 2

    base = args.base_url
    key = args.admin_key
    suffix = uuid.uuid4().hex[:8]

    # Sanity probe.
    probe = http("GET", base, "/api/status", key=key)
    if probe.status_code != 200:
        print(f"ERROR: {base}/api/status returned {probe.status_code}.")
        return 2

    results = [0, 0]  # [passed, failed]

    # Capture pre-test counter snapshot.  None if /api/debug/signals doesn't
    # exist (release build) — the per-counter assertions then skip.
    pre_url_rej = get_signal(base, key, "url_policy_rejections")
    pre_cred_rej = get_signal(base, key, "credentialed_plaintext_http_rejections")
    debug_signals_available = pre_url_rej is not None
    if debug_signals_available:
        info(f"Debug signals: url_policy_rejections={pre_url_rej}, "
             f"credentialed_plaintext_http_rejections={pre_cred_rej}")
    else:
        info("Debug signals unavailable (release build) — skipping counter checks.")

    # -- Reject: non-loopback http:// -----------------------------------
    header("Reject: non-loopback http://")
    assert_reject(base, key, f"test-policy-public-{suffix}",
                  "http://example.com/v1/chat", "",
                  "url_policy_violation", results)
    assert_reject(base, key, f"test-policy-rfc1918-{suffix}",
                  "http://192.168.1.5/v1/chat", "",
                  "url_policy_violation", results)
    assert_reject(base, key, f"test-policy-cgnat-{suffix}",
                  "http://100.64.1.1/v1/chat", "",
                  "url_policy_violation", results)

    # -- Reject: credentialed http:// even on loopback ------------------
    header("Reject: credentialed plain HTTP (loopback + key_name)")
    assert_reject(base, key, f"test-policy-credhttp-{suffix}",
                  "http://localhost:11434/v1/chat", "ollama",
                  "credentialed_plaintext_http", results)
    assert_reject(base, key, f"test-policy-credhttp-127-{suffix}",
                  "http://127.0.0.1:11434/v1/chat", "ollama",
                  "credentialed_plaintext_http", results)

    # -- Reject: forbidden schemes --------------------------------------
    header("Reject: forbidden schemes")
    assert_reject(base, key, f"test-policy-ws-{suffix}",
                  "ws://localhost/v1/chat", "",
                  "url_policy_violation", results)
    assert_reject(base, key, f"test-policy-file-{suffix}",
                  "file:///etc/passwd", "",
                  "url_policy_violation", results)
    assert_reject(base, key, f"test-policy-ftp-{suffix}",
                  "ftp://example.com/v1/chat", "",
                  "url_policy_violation", results)

    # -- Reject: malformed ----------------------------------------------
    header("Reject: malformed URLs")
    # Empty URL is caught by an earlier check (missing_url) before the policy
    # gate fires.  Both rejections are acceptable; the body's "error" field
    # should be set either way.
    r = create_interface(base, key, f"test-policy-empty-{suffix}", "", "")
    if expect(r.status_code == 400, f"empty url → HTTP 400 (got {r.status_code})", results):
        body = r.json()
        expect(body.get("error") in ("missing_url", "url_policy_violation"),
               f"  error={body.get('error')!r} (expected missing_url or url_policy_violation)",
               results)
    # Typo: "htp://" — caught by the policy gate as MalformedUrl
    # (no '://' substring matches → MalformedUrl ; matched but unknown scheme → SchemeRejected).
    assert_reject(base, key, f"test-policy-typo-{suffix}",
                  "htp://localhost:11434/v1/chat", "",
                  "url_policy_violation", results)

    # -- Accept: loopback http:// without key_name ----------------------
    header("Accept: loopback http:// (no key_name)")
    assert_accept(base, key, f"test-policy-ollama-localhost-{suffix}",
                  "http://localhost:11434/v1/chat/completions", "", results)
    assert_accept(base, key, f"test-policy-ollama-127-{suffix}",
                  "http://127.0.0.1:11434/v1/chat/completions", "", results)
    assert_accept(base, key, f"test-policy-ollama-127-subnet-{suffix}",
                  "http://127.5.6.7:11434/v1/chat/completions", "", results)
    # IPv6 loopback (::1 in bracketed form).
    assert_accept(base, key, f"test-policy-ollama-ipv6-{suffix}",
                  "http://[::1]:11434/v1/chat/completions", "", results)

    # -- Accept: https:// (regression check) ----------------------------
    header("Accept: https:// is unaffected (regression check)")
    assert_accept(base, key, f"test-policy-openai-{suffix}",
                  "https://api.openai.com/v1/chat/completions", "openai", results)

    # -- Counter assertions ---------------------------------------------
    if debug_signals_available:
        header("Debug signal counters")
        post_url_rej = get_signal(base, key, "url_policy_rejections")
        post_cred_rej = get_signal(base, key, "credentialed_plaintext_http_rejections")
        # Reject group counts:
        #   url_policy_violation: 3 non-loopback + 3 forbidden schemes
        #                       + 1 malformed-typo = 7
        #   credentialed_plaintext_http: 2 credentialed
        # The "empty url" case may or may not increment url_policy_rejections
        # depending on whether missing_url short-circuits first.  Accept both:
        # post_url_rej >= pre_url_rej + 7.
        delta_url = (post_url_rej or 0) - (pre_url_rej or 0)
        delta_cred = (post_cred_rej or 0) - (pre_cred_rej or 0)
        info(f"url_policy_rejections delta = {delta_url} (≥ 7 expected)")
        info(f"credentialed_plaintext_http_rejections delta = {delta_cred} (= 2 expected)")
        expect(delta_url >= 7, "url_policy_rejections incremented by ≥ 7", results)
        expect(delta_cred == 2, "credentialed_plaintext_http_rejections incremented by exactly 2", results)

    # -- Summary --------------------------------------------------------
    header("Summary")
    total = results[0] + results[1]
    passed_msg = f"{C.GREEN}{results[0]} passed{C.RESET}"
    failed_msg = f"{C.RED}{results[1]} failed{C.RESET}" if results[1] else f"{results[1]} failed"
    print(f"  {passed_msg}, {failed_msg}  ({total} total checks)")
    return 0 if results[1] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
