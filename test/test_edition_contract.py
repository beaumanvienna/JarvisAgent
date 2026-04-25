#!/usr/bin/env python3
"""
Edition Contract Tests — verifies the Engine/Studio route split and security features.

Tests cover:
  - Edition detection and capability flags
  - Route availability per edition (Engine vs Studio) post-§5i:
      * Routes in RegisterCommonRoutes() respond in both editions, role-gated.
      * Routes in RegisterStudioRoutes() return 404 on Engine.
  - MCP API key authentication (both editions — Studio's anonymous-localhost
    bypass was removed by §5i).
  - Security response headers (CSP, X-Frame-Options, Referrer-Policy, etc.)
  - Security audit log endpoint (GET /api/log/security)
  - Auth lockout response format, RBAC, TLS status field

Usage:
    python3 test/test_edition_contract.py                  # auto-detect edition
    python3 test/test_edition_contract.py --edition engine  # assert Engine
    python3 test/test_edition_contract.py --edition studio  # assert Studio
    python3 test/test_edition_contract.py --base-url http://host:port
    python3 test/test_edition_contract.py --token mcp_...   # MCP admin key

The script assumes JarvisAgent is already running. An admin MCP API key is
required for both editions (--token or J9T_TOKEN env var).
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent

try:
    import requests
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
except ImportError:
    print("ERROR: 'requests' package not found. Install with: pip install requests")
    sys.exit(1)

try:
    import websocket as ws_client
    HAS_WEBSOCKET = True
except ImportError:
    HAS_WEBSOCKET = False

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

def ok(msg):     print(f"  {C.GREEN}\u2713{C.RESET} {msg}")
def fail(msg):   print(f"  {C.RED}\u2717{C.RESET} {msg}")
def info(msg):   print(f"  {C.CYAN}\u2139{C.RESET} {msg}")
def warn(msg):   print(f"  {C.YELLOW}\u26a0{C.RESET} {msg}")
def header(msg): print(f"\n{C.BOLD}{C.CYAN}{'\u2500'*60}\n  {msg}\n{'\u2500'*60}{C.RESET}")

# ---------------------------------------------------------------------------
# Test framework
# ---------------------------------------------------------------------------
class TestRunner:
    def __init__(self, base_url, token=None):
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.verify_ssl = not base_url.startswith("https://localhost")
        self.edition = None  # set by main() once /api/status has been polled
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def _url(self, path):
        return f"{self.base_url}{path}"

    def _ws_url(self, path):
        return self._url(path).replace("http://", "ws://").replace("https://", "wss://")

    def _auth_headers(self):
        if self.token:
            return {"Authorization": f"Bearer {self.token}"}
        return {}

    def get(self, path, **kwargs):
        headers = {**self._auth_headers(), **kwargs.pop("headers", {})}
        return requests.get(self._url(path), timeout=10, headers=headers, verify=self.verify_ssl, **kwargs)

    def post(self, path, **kwargs):
        headers = {**self._auth_headers(), **kwargs.pop("headers", {})}
        return requests.post(self._url(path), timeout=10, headers=headers, verify=self.verify_ssl, **kwargs)

    def put(self, path, **kwargs):
        headers = {**self._auth_headers(), **kwargs.pop("headers", {})}
        return requests.put(self._url(path), timeout=10, headers=headers, verify=self.verify_ssl, **kwargs)

    def delete(self, path, **kwargs):
        headers = {**self._auth_headers(), **kwargs.pop("headers", {})}
        return requests.delete(self._url(path), timeout=10, headers=headers, verify=self.verify_ssl, **kwargs)

    def get_no_auth(self, path, **kwargs):
        """GET without auth token (for testing unauthenticated access)."""
        return requests.get(self._url(path), timeout=10, verify=self.verify_ssl, **kwargs)

    def post_no_auth(self, path, **kwargs):
        """POST without auth token (for testing unauthenticated access)."""
        return requests.post(self._url(path), timeout=10, verify=self.verify_ssl, **kwargs)

    def assert_status(self, method, path, expected_status, label=None, **kwargs):
        """Assert that a request returns the expected HTTP status code.
        expected_status can be an int or a set/list of acceptable codes."""
        tag = label or f"{method.upper()} {path}"
        acceptable = expected_status if isinstance(expected_status, (set, list, tuple)) else {expected_status}
        try:
            fn = getattr(self, method.lower())
            r = fn(path, **kwargs)
            if r.status_code in acceptable:
                ok(f"{tag} -> {r.status_code}")
                self.passed += 1
                return r
            else:
                fail(f"{tag} -> {r.status_code} (expected {acceptable})")
                self.failed += 1
                return r
        except requests.ConnectionError as e:
            if expected_status == 404:
                # Connection refused counts as "not available"
                ok(f"{tag} -> connection refused (route absent)")
                self.passed += 1
                return None
            else:
                fail(f"{tag} -> connection error: {e}")
                self.failed += 1
                return None
        except Exception as e:
            fail(f"{tag} -> error: {e}")
            self.failed += 1
            return None

    def assert_json_field(self, data, field, expected, label=None):
        """Assert a JSON field has the expected value."""
        tag = label or f"field '{field}'"
        actual = data
        for key in field.split("."):
            if isinstance(actual, dict):
                actual = actual.get(key)
            else:
                actual = None
                break
        if actual == expected:
            ok(f"{tag}: {json.dumps(actual)}")
            self.passed += 1
        else:
            fail(f"{tag}: {json.dumps(actual)} (expected {json.dumps(expected)})")
            self.failed += 1

    def assert_ws_connectable(self, path, expect_connectable, label=None, with_auth=False):
        """Assert that a WebSocket endpoint is/isn't connectable.

        with_auth=True passes the admin MCP token as an Authorization header on
        the WS upgrade so the .onaccept() handshake gate sees a valid credential
        — used for routes that should accept *authenticated* clients (the only
        legitimate use of /ws and /ws/assistant after the §5i refactor)."""
        tag = label or f"WS {path}"
        if not HAS_WEBSOCKET:
            warn(f"{tag} -> skipped (websocket-client not installed)")
            self.skipped += 1
            return

        url = self._ws_url(path)
        try:
            ws_opts = {}
            if not self.verify_ssl:
                import ssl
                ws_opts["sslopt"] = {"cert_reqs": ssl.CERT_NONE}
            if with_auth and self.token:
                ws_opts["header"] = [f"Authorization: Bearer {self.token}"]
            sock = ws_client.create_connection(url, timeout=5, **ws_opts)
            sock.close()
            if expect_connectable:
                ok(f"{tag} -> connected")
                self.passed += 1
            else:
                fail(f"{tag} -> connected (expected refused)")
                self.failed += 1
        except Exception:
            if not expect_connectable:
                ok(f"{tag} -> refused (as expected)")
                self.passed += 1
            else:
                fail(f"{tag} -> refused (expected connectable)")
                self.failed += 1

    def summary(self):
        total = self.passed + self.failed + self.skipped
        color = C.GREEN if self.failed == 0 else C.RED
        print(f"\n  {C.BOLD}{color}{self.passed} passed, {self.failed} failed"
              f"{f', {self.skipped} skipped' if self.skipped else ''}"
              f" ({total} total){C.RESET}\n")
        return self.failed == 0


# ---------------------------------------------------------------------------
# Shared tests (both editions)
# ---------------------------------------------------------------------------
def test_common(t, status_data):
    header("Common — both editions")

    # /api/status structure
    t.assert_json_field(status_data, "ok", True, "status.ok")
    edition = status_data.get("edition")
    if edition in ("engine", "studio"):
        ok(f"status.edition: \"{edition}\"")
        t.passed += 1
    else:
        fail(f"status.edition: {edition!r} (expected 'engine' or 'studio')")
        t.failed += 1

    caps = status_data.get("capabilities")
    if isinstance(caps, dict):
        ok(f"status.capabilities present ({len(caps)} keys)")
        t.passed += 1
        for key in ("workflow_crud", "workflow_run_endpoint", "ai_assistant",
                     "ai_jcwf", "settings_api", "log_analyze",
                     "workflow_versions", "workflow_reload"):
            if key in caps and isinstance(caps[key], bool):
                ok(f"  capabilities.{key}: {caps[key]}")
                t.passed += 1
            else:
                fail(f"  capabilities.{key}: missing or not boolean")
                t.failed += 1
    else:
        fail("status.capabilities missing or not an object")
        t.failed += 1

    # Common routes should return 200 (admin token required in both editions post-§5i).
    t.assert_status("get", "/api/workflows", 200)
    t.assert_status("get", "/api/workflow-runs/active", 200)
    t.assert_status("get", "/api/workflow-runs/last", 200)
    t.assert_status("get", "/api/log", 200)
    t.assert_status("get", "/", 200, label="GET / (dashboard, public)")

    # /ws upgrade requires a session cookie or bearer token on the handshake in
    # both editions post-§5i.  Anonymous handshake must reject; authenticated
    # handshake must connect.
    t.assert_ws_connectable("/ws", False, "WS /ws (anonymous, must reject)")
    t.assert_ws_connectable("/ws", True,  "WS /ws (authenticated)", with_auth=True)


# ---------------------------------------------------------------------------
# Engine-specific tests
# ---------------------------------------------------------------------------
def test_engine(t, status_data):
    header("Engine — Studio-only routes absent (404), shared routes role-gated")

    # Edition & capabilities post-§5i: workflow_run_endpoint, settings_api,
    # log_analyze, workflow_versions, workflow_reload all True in Engine because
    # the underlying routes moved to RegisterCommonRoutes(). Only
    # workflow_crud / ai_assistant / ai_jcwf remain Studio-only.
    t.assert_json_field(status_data, "edition", "engine")
    t.assert_json_field(status_data, "capabilities.workflow_crud", False)
    t.assert_json_field(status_data, "capabilities.ai_assistant", False)
    t.assert_json_field(status_data, "capabilities.ai_jcwf", False)
    t.assert_json_field(status_data, "capabilities.workflow_run_endpoint", True)
    t.assert_json_field(status_data, "capabilities.settings_api", True)
    t.assert_json_field(status_data, "capabilities.log_analyze", True)
    t.assert_json_field(status_data, "capabilities.workflow_versions", True)
    t.assert_json_field(status_data, "capabilities.workflow_reload", True)

    # ---- Studio-only routes still absent in Engine ----
    ABSENT = {404, 405}
    t.assert_status("post", "/api/workflows", ABSENT,
                    json={"id": "__test__", "tasks": []})
    t.assert_status("put", "/api/workflows/__test__", ABSENT,
                    json={"id": "__test__", "tasks": []})
    t.assert_status("delete", "/api/workflows/__test__", ABSENT)
    t.assert_status("post", "/api/workflows/validate", ABSENT,
                    json={"id": "__test__", "tasks": []})
    t.assert_status("get", "/api/workflows/__test__/validate", 404)
    t.assert_status("get", "/api/scripts/check", 404)
    t.assert_status("get", "/api/files/check", 404)
    t.assert_status("post", "/api/chat", 404, json={"message": "test"})
    t.assert_status("get", "/editor", 404)
    t.assert_ws_connectable("/ws/assistant", False, "WS /ws/assistant (absent)")

    # ---- Routes moved from Studio to Common (registered, role-gated) ----
    # Without auth → 401 (route exists, auth required). Distinguishes from 404
    # which would mean the route is absent.
    NEEDS_AUTH = {401, 403}
    r = t.get_no_auth("/api/workflows/reload" if False else "/api/log/analyze-last-run")
    if r.status_code in NEEDS_AUTH:
        ok(f"GET /api/log/analyze-last-run (no auth) -> {r.status_code} (route exists)")
        t.passed += 1
    else:
        fail(f"GET /api/log/analyze-last-run (no auth) -> {r.status_code} (expected 401/403)")
        t.failed += 1

    for path in ["/api/settings/config", "/api/settings/ai-interfaces",
                 "/api/settings/providers", "/api/connections",
                 "/api/workflows/__test__/versions"]:
        r = t.get_no_auth(path)
        if r.status_code in NEEDS_AUTH:
            ok(f"GET {path} (no auth) -> {r.status_code} (route exists)")
            t.passed += 1
        else:
            fail(f"GET {path} (no auth) -> {r.status_code} (expected 401/403)")
            t.failed += 1

    # With admin token: 200 for the routes that don't need a workflow id.
    t.assert_status("get", "/api/settings/config", 200,
                    label="/api/settings/config (admin)")
    t.assert_status("get", "/api/settings/ai-interfaces", 200,
                    label="/api/settings/ai-interfaces (admin)")
    t.assert_status("get", "/api/settings/providers", 200,
                    label="/api/settings/providers (admin)")
    t.assert_status("get", "/api/connections", 200,
                    label="/api/connections (admin)")

    # POST /api/workflows/__test__/run: route registered (Common), workflow
    # doesn't exist → expect 404 from the handler (not 401).
    t.assert_status("post", "/api/workflows/__test__/run", {404, 400},
                    label="run nonexistent workflow (admin)")

    # Shared bootstrap.
    t.assert_status("get", "/api/settings/keys/status", 200,
                    label="/api/settings/keys/status (bootstrap)")


# ---------------------------------------------------------------------------
# Engine auth tests
# ---------------------------------------------------------------------------
def test_engine_auth(t):
    header("Engine — admin auth (bearer token)")

    if not t.token:
        warn("No admin token available — skipping auth tests")
        t.skipped += 1
        return

    # ---- Valid token tests FIRST (before any failed attempts) ----
    t.assert_status("get", "/api/workflows", 200, label="GET /api/workflows (valid token)")
    t.assert_status("get", "/api/workflow-runs/active", 200, label="GET /api/workflow-runs/active (valid token)")
    t.assert_status("get", "/api/log", 200, label="GET /api/log (valid token)")

    # Public endpoints should work without token.
    r = t.get_no_auth("/api/status")
    if r.status_code == 200:
        ok("GET /api/status -> 200 (no token, public)")
        t.passed += 1
    else:
        fail(f"GET /api/status -> {r.status_code} (expected 200, public)")
        t.failed += 1

    # Admin endpoints should return 401 or 403 without token.
    # (403 can occur if the IP has prior failures from dashboard polling.)
    for path in ["/api/workflows", "/api/workflow-runs/active", "/api/log"]:
        r = t.get_no_auth(path)
        if r.status_code in (401, 403):
            ok(f"GET {path} -> {r.status_code} (no token, rejected)")
            t.passed += 1
        else:
            fail(f"GET {path} -> {r.status_code} (expected 401 or 403)")
            t.failed += 1

    r = t.post_no_auth("/api/shutdown")
    if r.status_code in (401, 403):
        ok(f"POST /api/shutdown -> {r.status_code} (no token, rejected)")
        t.passed += 1
    else:
        fail(f"POST /api/shutdown -> {r.status_code} (expected 401 or 403)")
        t.failed += 1

    # Wrong token should return 403.
    r = requests.get(t._url("/api/workflows"), timeout=10, verify=t.verify_ssl,
                     headers={"Authorization": "Bearer wrong_token_value"})
    if r.status_code == 403:
        ok("GET /api/workflows -> 403 (wrong token)")
        t.passed += 1
    else:
        fail(f"GET /api/workflows -> {r.status_code} (expected 403)")
        t.failed += 1

    # Valid token should still work after wrong-token attempt
    # (successful auth clears the failure count for this IP).
    t.assert_status("get", "/api/workflows", 200, label="GET /api/workflows (valid token, after bad attempt)")


# ---------------------------------------------------------------------------
# Security tests (both editions, but auth features only active on Engine)
# ---------------------------------------------------------------------------
def test_security_headers(t):
    """Verify security headers are present on all responses."""
    header("Security — response headers")

    r = t.get_no_auth("/api/status")
    h = r.headers

    expected_headers = {
        "X-Frame-Options": "DENY",
        "X-Content-Type-Options": "nosniff",
        "Content-Security-Policy": None,  # just check presence
        "Referrer-Policy": "strict-origin-when-cross-origin",
        "Permissions-Policy": None,  # just check presence
    }

    for name, expected_value in expected_headers.items():
        actual = h.get(name)
        if actual is None:
            fail(f"Header '{name}' missing")
            t.failed += 1
        elif expected_value is None:
            ok(f"Header '{name}' present: {actual[:60]}...")
            t.passed += 1
        elif actual == expected_value:
            ok(f"Header '{name}': {actual}")
            t.passed += 1
        else:
            fail(f"Header '{name}': {actual!r} (expected {expected_value!r})")
            t.failed += 1

    # CSP should contain key directives
    csp = h.get("Content-Security-Policy", "")
    for directive in ["default-src 'self'", "script-src 'self'", "connect-src 'self' ws: wss:"]:
        if directive in csp:
            ok(f"CSP contains: {directive}")
            t.passed += 1
        else:
            fail(f"CSP missing directive: {directive}")
            t.failed += 1

    # JSON endpoints should also have security headers
    r2 = t.get("/api/status")
    if r2.headers.get("X-Frame-Options") == "DENY":
        ok("JSON endpoint has X-Frame-Options")
        t.passed += 1
    else:
        fail("JSON endpoint missing X-Frame-Options")
        t.failed += 1


def test_security_log_endpoint(t):
    """Verify the security log endpoint exists and is accessible."""
    header("Security — audit log endpoint")

    # GET /api/log/security should return 200 or 404 (no log yet), not 401/500
    r = t.get("/api/log/security")
    if r.status_code in (200, 404):
        ok(f"GET /api/log/security -> {r.status_code}")
        t.passed += 1
    else:
        fail(f"GET /api/log/security -> {r.status_code} (expected 200 or 404)")
        t.failed += 1

    if r.status_code == 200:
        data = r.json()
        if data.get("ok") is True and "lines" in data and "byteOffset" in data:
            ok("Security log response has correct structure (ok, lines, byteOffset)")
            t.passed += 1
        else:
            fail(f"Security log response structure incorrect: {list(data.keys())}")
            t.failed += 1


def test_engine_security(t):
    """Engine-specific security tests: RBAC, security log, TLS status, auth error format."""
    header("Engine — security features")

    if not t.token:
        warn("No admin token — skipping Engine security tests")
        t.skipped += 1
        return

    # ---- First: ensure valid token works (clears any prior lockout) ----
    r = t.get("/api/workflows")
    if r.status_code == 200:
        ok("Valid token works (lockout cleared)")
        t.passed += 1
    else:
        fail(f"Valid token -> {r.status_code} (IP may be locked out — restart Engine to reset)")
        t.failed += 1
        warn("Remaining security tests may fail due to lockout")

    # ---- Auth error response format ----
    r = requests.get(t._url("/api/workflows"), timeout=10, verify=t.verify_ssl,
                     headers={"Authorization": "Bearer wrong_token_12345"})
    if r.status_code == 403:
        data = r.json()
        if data.get("ok") is False and data.get("error") in ("forbidden", "locked_out"):
            ok(f"Wrong token -> 403 with {{ok:false, error:'{data['error']}'}}")
            t.passed += 1
        else:
            fail(f"Wrong token response format unexpected: {data}")
            t.failed += 1
    else:
        fail(f"Wrong token -> {r.status_code} (expected 403)")
        t.failed += 1

    # Clear lockout by authenticating with valid token
    t.get("/api/workflows")

    # ---- Security log endpoint is admin-only ----
    r = t.get("/api/log/security")
    if r.status_code in (200, 404):
        ok(f"GET /api/log/security with admin token -> {r.status_code}")
        t.passed += 1
    else:
        fail(f"GET /api/log/security with admin token -> {r.status_code}")
        t.failed += 1

    # Without token, should be 401 or 403 (403 if prior failures accumulated)
    r = t.get_no_auth("/api/log/security")
    if r.status_code in (401, 403):
        ok(f"GET /api/log/security without token -> {r.status_code} (rejected)")
        t.passed += 1
    else:
        fail(f"GET /api/log/security without token -> {r.status_code} (expected 401 or 403)")
        t.failed += 1

    # Clear lockout again
    t.get("/api/workflows")

    # ---- Status includes tls field ----
    r = t.get_no_auth("/api/status")
    if r.status_code == 200:
        data = r.json()
        if "tls" in data and isinstance(data["tls"], bool):
            ok(f"status.tls present: {data['tls']}")
            t.passed += 1
        else:
            fail("status.tls missing or not boolean")
            t.failed += 1

    # ---- Shutdown without token -> rejected ----
    r = t.post_no_auth("/api/shutdown")
    if r.status_code in (401, 403, 429):
        ok(f"POST /api/shutdown without token -> {r.status_code} (rejected)")
        t.passed += 1
    else:
        fail(f"POST /api/shutdown without token -> {r.status_code} (expected 401, 403, or 429)")
        t.failed += 1

    # Brief pause to let rate limit tokens refill, then confirm valid token still works.
    time.sleep(1)
    t.assert_status("get", "/api/workflows", 200, label="GET /api/workflows (valid token, end of security tests)")


# ---------------------------------------------------------------------------
# Studio-specific tests
# ---------------------------------------------------------------------------
def test_studio(t, status_data):
    header("Studio — all routes must be present")

    # Edition & capabilities — every flag True in Studio.
    t.assert_json_field(status_data, "edition", "studio")
    t.assert_json_field(status_data, "capabilities.workflow_crud", True)
    t.assert_json_field(status_data, "capabilities.workflow_run_endpoint", True)
    t.assert_json_field(status_data, "capabilities.ai_assistant", True)
    t.assert_json_field(status_data, "capabilities.ai_jcwf", True)
    t.assert_json_field(status_data, "capabilities.settings_api", True)
    t.assert_json_field(status_data, "capabilities.log_analyze", True)
    t.assert_json_field(status_data, "capabilities.workflow_versions", True)
    t.assert_json_field(status_data, "capabilities.workflow_reload", True)

    # CRUD routes present (we use safe methods / non-existent IDs to avoid side effects)
    t.assert_status("post", "/api/workflows/reload", 200)

    # Validation endpoint present (may return 400 for invalid body, but not 404)
    r = t.post("/api/workflows/validate", json={"id": "__test__", "tasks": []})
    if r.status_code != 404:
        ok(f"POST /api/workflows/validate -> {r.status_code} (not 404)")
        t.passed += 1
    else:
        fail(f"POST /api/workflows/validate -> 404 (should be registered)")
        t.failed += 1

    # Settings API present
    t.assert_status("get", "/api/settings/config", 200)
    t.assert_status("get", "/api/settings/ai-interfaces", 200)
    t.assert_status("get", "/api/settings/keys/status", 200)
    t.assert_status("get", "/api/settings/providers", 200)

    # Script check present (no params → likely 200 or 400, not 404)
    r = t.get("/api/scripts/check")
    if r.status_code != 404:
        ok(f"GET /api/scripts/check -> {r.status_code} (not 404)")
        t.passed += 1
    else:
        fail(f"GET /api/scripts/check -> 404 (should be registered)")
        t.failed += 1

    # Editor UI present
    t.assert_status("get", "/editor", 200)

    # No /api/chat REST route — assistant chat is delivered over the
    # /ws/assistant WebSocket channel via {"type":"chat"} messages.  The
    # capability flag (ai_assistant=true) and the WS handshake test below
    # are the authoritative checks that the assistant feature is present.

    # Log analysis present (may return error without recent run, but not 404)
    r = t.get("/api/log/analyze-last-run")
    if r.status_code != 404:
        ok(f"GET /api/log/analyze-last-run -> {r.status_code} (not 404)")
        t.passed += 1
    else:
        fail(f"GET /api/log/analyze-last-run -> 404 (should be registered)")
        t.failed += 1

    # Assistant WebSocket — anonymous handshake must reject (auth funnel
    # at .onaccept), authenticated upgrade must succeed.
    t.assert_ws_connectable("/ws/assistant", False, "WS /ws/assistant (anonymous, must reject)")
    t.assert_ws_connectable("/ws/assistant", True,  "WS /ws/assistant (authenticated)", with_auth=True)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Edition Contract Tests")
    parser.add_argument("--edition", choices=["engine", "studio"],
                        help="Assert specific edition (default: auto-detect)")
    parser.add_argument("--base-url", default="http://localhost:8080",
                        help="JarvisAgent base URL")
    parser.add_argument("--token", default=None,
                        help="MCP admin API key for Engine auth (default: J9T_TOKEN env var)")
    args = parser.parse_args()

    # MCP admin key — passed via --token or J9T_TOKEN env var (same convention as the
    # MCP sidecar). There is no file-based token — ripped out alongside the legacy
    # admin bearer-token system.
    token = args.token or os.environ.get("J9T_TOKEN") or ""
    if token and not token.startswith("mcp_"):
        warn(f"Provided token does not start with 'mcp_' — Engine auth will reject it")

    t = TestRunner(args.base_url, token=token)

    print(f"\n{C.BOLD}Edition Contract Tests{C.RESET}")
    info(f"Base URL: {args.base_url}")
    if not HAS_WEBSOCKET:
        warn("websocket-client not installed — WebSocket tests will be skipped")
        info("Install with: pip3 install websocket-client")

    # Health check
    try:
        r = t.get("/api/status")
        r.raise_for_status()
        status_data = r.json()
    except Exception as e:
        fail(f"Cannot reach JarvisAgent at {args.base_url}: {e}")
        sys.exit(1)

    detected_edition = status_data.get("edition", "unknown")
    ok(f"Connected — edition: {detected_edition}")

    # Verify requested edition matches detected
    if args.edition and args.edition != detected_edition:
        fail(f"Requested --edition {args.edition} but server reports '{detected_edition}'")
        sys.exit(1)

    edition = args.edition or detected_edition
    t.edition = edition

    # Run tests
    test_common(t, status_data)
    test_security_headers(t)

    if edition == "engine":
        test_engine(t, status_data)
        test_engine_auth(t)
        test_security_log_endpoint(t)
        test_engine_security(t)
    elif edition == "studio":
        test_studio(t, status_data)
        test_security_log_endpoint(t)
    else:
        fail(f"Unknown edition: {edition!r}")
        sys.exit(1)

    # Summary
    header("RESULTS")
    success = t.summary()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
