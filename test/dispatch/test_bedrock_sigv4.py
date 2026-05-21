#!/usr/bin/env python3
"""
Hermetic SigV4 KAT — verify the Authorization header that AiRequestPool +
SigV4Signer would emit for an AWS Bedrock dispatch matches a locked
ground-truth signature.  No real AWS calls: MockTransport replays a canned
fixture and captures the auth-signer output for /api/debug/signals to surface.

This is the end-to-end test the awsSigV4.cpp::RunSelfTest #4 self-test cannot
provide — selfTest hand-rolls the Inputs struct, whereas this exercises the
full typed-credential-through-QueryData wiring (AwsCredential snapshot →
m_AwsCredential → SigV4Signer::Apply → captured header).

Bootstrap pattern (memory/feedback_crypto_test_bootstrap_pattern.md):
  1. Placeholder EXPECTED_SIGNATURE_HEX on first run.
  2. Test fails; failure message includes the captured signature.
  3. Lock that captured value into EXPECTED_SIGNATURE_HEX once the inputs
     (credential, interface URL/model, JCWF inputs, AmzDate override) settle.

Inputs match awsSigV4.cpp::RunSelfTest #3+#4 chain (AKIDEXAMPLE access key,
AWS-published example secret, us-east-1, bedrock service, 2024-01-01T12:00:00Z
AmzDate) so the signing-key derivation pipeline this test exercises overlaps
with the at-startup self-test's KAT.  Any regression in canonical-request
assembly or HMAC chain breaks BOTH tests at once.

Requires DEBUG build (the /api/debug/signals endpoint is only registered in
debug builds, see feedback_debug_signals).
"""
import os
import sys
import time

import urllib3

try:
    import requests
except ImportError:
    print("ERROR: requests package missing (pip install requests)")
    sys.exit(1)

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

BASE = os.environ.get("J9T_BASE_URL", "https://localhost:8443")
TOKEN = os.environ.get("J9T_TOKEN")
HEADERS = {"Authorization": f"Bearer {TOKEN}"} if TOKEN else {}

SUFFIX = str(int(time.time()))
PROVIDER_NAME = f"sigv4_kat_bedrock_{SUFFIX}"
INTERFACE_NAME = f"mock_api5_sigv4_kat_{SUFFIX}"
WF_ID = f"adhoc_sigv4_kat_{SUFFIX}"

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_PATH = os.path.join(REPO_ROOT, "test", "dispatch", "fixtures", "api5", "sigv4_kat.json")

EXPECTED_AUTH_PREFIX = (
    "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20240101/us-east-1/bedrock/aws4_request, "
    "SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature="
)
# Locked 2026-05-19 from the first successful end-to-end dispatch.  Pairs with
# awsSigV4.cpp::RunSelfTest #2 (signing-key derivation against AWS-published
# vector) as the independent reference vector required by
# feedback_crypto_test_bootstrap_pattern.  Any regression in canonical-request
# assembly, JSON body composition (BedrockRequestBuilder), URL resolution, or
# the HMAC chain breaks this constant — the failure message prints the new
# capture so re-locking is one copy-paste.
EXPECTED_SIGNATURE_HEX = "1a6d6607ae8458641685888fa012825e591fb38ca4db178eab28d9a9b07ae021"


def create_provider():
    body = {
        "name": PROVIDER_NAME,
        "credential_type": "aws",
        "display_name": "Bedrock SigV4 KAT (hermetic test)",
        "endpoint": "https://bedrock-runtime.us-east-1.amazonaws.com",
        "default_model": "anthropic.claude-3-haiku-20240307-v1:0",
        "api_type": "API5",
        "access_key_id": "AKIDEXAMPLE",
        "secret_access_key": "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY",
        "region": "us-east-1",
    }
    r = requests.post(f"{BASE}/api/settings/providers", json=body, headers=HEADERS,
                      verify=False, timeout=10)
    return r.status_code in (200, 201, 409), r


def delete_provider():
    try:
        requests.delete(f"{BASE}/api/settings/providers/{PROVIDER_NAME}",
                        headers=HEADERS, verify=False, timeout=10)
    except Exception:
        pass


def create_interface():
    body = {
        "name": INTERFACE_NAME,
        "description": "SigV4 KAT mock — hermetic capture only",
        "url": "https://bedrock-runtime.us-east-1.amazonaws.com",
        "model": "anthropic.claude-3-haiku-20240307-v1:0",
        "api_type": "API5",
        "key_name": PROVIDER_NAME,
        "is_mock": True,
        "fixture_path": FIXTURE_PATH,
        "rate_limit": {"max_retries_429": 0, "max_retries_transient": 0},
    }
    r = requests.post(f"{BASE}/api/settings/ai-interfaces", json=body, headers=HEADERS,
                      verify=False, timeout=10)
    return r.status_code in (200, 201, 409), r


def delete_interface():
    try:
        requests.delete(f"{BASE}/api/settings/ai-interfaces/{INTERFACE_NAME}",
                        headers=HEADERS, verify=False, timeout=10)
    except Exception:
        pass


def build_jcwf():
    return {
        "version": "1.0",
        "id": WF_ID,
        "label": "SigV4 KAT",
        "triggers": [{"type": "manual", "id": "manual", "enabled": True, "params": {}}],
        "tasks": {
            "echo": {
                "id": "echo",
                "type": "ai_call",
                "label": "Bedrock SigV4 dispatch (captured by MockTransport)",
                "mode": "single",
                "working_directory": f"../../queue/{WF_ID}/01_echo",
                "params": {"provider": INTERFACE_NAME},
                "queue_binding": {
                    "stng_files": [{"path": "STNG_x.txt", "content": ""}],
                    "task_files": [{"path": "TASK_x.txt", "content": "hi"}],
                    "cntx_files": [{"path": "CNTX_x.txt", "content": ""}],
                    "prob_files": [{"path": "PROB_x.txt", "content": ""}],
                },
            }
        },
    }


def poll_terminal(run_id, timeout_s=20):
    start = time.time()
    while time.time() - start < timeout_s:
        r = requests.get(f"{BASE}/api/workflow-runs/{run_id}",
                         headers=HEADERS, verify=False, timeout=10)
        if r.status_code == 200:
            body = r.json()
            run = body.get("run") if isinstance(body.get("run"), dict) else body
            state = run.get("state")
            if state in ("succeeded", "failed", "cancelled"):
                return state
        time.sleep(0.2)
    return None


def get_last_authorization():
    r = requests.get(f"{BASE}/api/debug/signals", headers=HEADERS, verify=False, timeout=10)
    if r.status_code == 404:
        return None, ("/api/debug/signals returned 404 — this test requires a DEBUG build "
                      "(the endpoint is compiled out of release per feedback_debug_signals)")
    if r.status_code != 200:
        return None, f"/api/debug/signals returned {r.status_code}: {r.text[:200]}"
    sigs = r.json().get("signals", {}).get("last_mock_signatures", [])
    if not sigs:
        return None, "no captured signatures in /api/debug/signals['last_mock_signatures']"
    latest = sigs[-1]  # most-recent at the tail (FIFO ring)
    for h in latest.get("headers", []):
        if h.startswith("Authorization:"):
            return h, None
    return None, f"no Authorization header in captured headers: {latest.get('headers')}"


def main():
    if not TOKEN:
        print("FAIL: J9T_TOKEN env var not set")
        return 1

    if not os.path.isfile(FIXTURE_PATH):
        print(f"FAIL: fixture not found at {FIXTURE_PATH}")
        return 1

    try:
        ok, r = create_provider()
        if not ok:
            print(f"FAIL: create_provider returned {r.status_code}: {r.text[:200]}")
            return 1

        ok, r = create_interface()
        if not ok:
            print(f"FAIL: create_interface returned {r.status_code}: {r.text[:200]}")
            return 1

        wf = build_jcwf()
        r = requests.post(f"{BASE}/api/workflows/run-adhoc",
                          json={"jcwf": wf, "cleanup_policy": "ttl_1h"},
                          headers=HEADERS, verify=False, timeout=10)
        if r.status_code not in (200, 201, 202):
            print(f"FAIL: /api/workflows/run-adhoc returned {r.status_code}: {r.text[:200]}")
            return 1
        body = r.json()
        run_id = body.get("runId") or body.get("run_id")
        if not run_id:
            print(f"FAIL: no runId in run-adhoc response: {body!r}")
            return 1

        terminal = poll_terminal(run_id)
        if terminal is None:
            print(f"FAIL: run {run_id} never reached terminal state in 20s")
            return 1

        auth_header, err = get_last_authorization()
        if auth_header is None:
            print(f"FAIL: {err}")
            return 1

        auth_value = auth_header.split(":", 1)[1].strip()

        if not auth_value.startswith(EXPECTED_AUTH_PREFIX):
            print("FAIL: Authorization header structure does not match expected prefix.")
            print(f"  expected_prefix: {EXPECTED_AUTH_PREFIX!r}")
            print(f"  got:             {auth_value!r}")
            return 1

        sig_hex = auth_value[len(EXPECTED_AUTH_PREFIX):]
        if sig_hex != EXPECTED_SIGNATURE_HEX:
            print("FAIL: signature hex mismatch (or first-run bootstrap).")
            print(f"  got_signature: {sig_hex}")
            print(f"  expected:      {EXPECTED_SIGNATURE_HEX}")
            print()
            print("BOOTSTRAP: if inputs are settled, replace EXPECTED_SIGNATURE_HEX with the got_signature value.")
            return 1

        print(f"OK: SigV4 captured signature matches locked KAT (runId={run_id}, terminal={terminal})")
        print(f"    Authorization: {auth_value}")
        return 0
    finally:
        delete_interface()
        delete_provider()


if __name__ == "__main__":
    sys.exit(main())
