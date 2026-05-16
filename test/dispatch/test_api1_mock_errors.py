#!/usr/bin/env python3
"""
Per-fixture fault test for the API1 (OpenAI Chat) parser path through MockTransport.

Runs each error fixture in `test/dispatch/fixtures/api1/` against an `is_mock=true`
interface; asserts the dispatcher reached the expected terminal state, the PROV
sidecar records `mocked: true`, and the log captured the failure with the runId
on an [error]/[critical] line.  malformed_utf8 is the success-path renderer
stress; the run succeeds and the log + output bytes stay valid UTF-8.

Today the dispatcher's error log carries only the HTTP status; the body
discriminator (insufficient_quota / rate_limit_error / authentication_error /
server_error) lives in the parsed body but isn't on the error line yet.  When
that gap closes, tighten `expected_log_substring` from `(429)` to
`insufficient_quota` etc. below.

Runs against a live JarvisAgent instance (default https://localhost:8443).
Requires an MCP admin key via --token or the J9T_TOKEN env var.
"""

import sys
from pathlib import Path

from _per_api_fault_helpers import ApiTestPlan, REPO_ROOT, parse_args_and_run


CASES = [
    # Sitting-5 Workstream A enriched the OnRequestFailed ERROR with the body
    # discriminator + semantic category, so each driver asserts on the body's
    # `type` field rather than the HTTP status.  Distinguishes billing vs
    # throttle (both HTTP 429) at the log layer.
    {
        "name": "error_billing",
        "fixture": "error_billing.json",
        "expected_run_state": "failed",
        "expected_log_substring": "insufficient_quota",
        "expected_http_status_in_meta": 429,
    },
    {
        "name": "error_throttle",
        "fixture": "error_throttle.json",
        "expected_run_state": "failed",
        "expected_log_substring": "rate_limit_error",
        "expected_http_status_in_meta": 429,
    },
    {
        "name": "error_auth",
        "fixture": "error_auth.json",
        "expected_run_state": "failed",
        "expected_log_substring": "authentication_error",
        "expected_http_status_in_meta": 401,
    },
    {
        "name": "error_overload",
        "fixture": "error_overload.json",
        "expected_run_state": "failed",
        "expected_log_substring": "server_error",
        "expected_http_status_in_meta": 503,
    },
    {
        "name": "malformed_utf8",
        "fixture": "malformed_utf8.json",
        "expected_run_state": "succeeded",
        "expected_log_substring": None,
        "expected_http_status_in_meta": None,
    },
    {
        "name": "truncated_response",
        "fixture": "truncated_response.json",
        "expected_run_state": "failed",
        "expected_log_substring": None,
        "expected_http_status_in_meta": None,
    },
]


PLAN = ApiTestPlan(
    api_type="API1",
    fixture_dir=REPO_ROOT / "test" / "dispatch" / "fixtures" / "api1",
    cases=CASES,
    interface_prefix="mock_api1_fault_",
)


if __name__ == "__main__":
    sys.exit(parse_args_and_run(PLAN, "api1"))
