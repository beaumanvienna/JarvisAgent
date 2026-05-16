#!/usr/bin/env python3
"""
Per-fixture fault test for API2 (OpenAI Responses) through MockTransport.

Same shape as `test_api1_mock_errors.py`; the OpenAI Responses error envelope
is byte-for-byte identical to Chat-Completions, so all four error fixtures are
the same.  The success/malformed/truncated fixtures use the Responses body
shape (`output[0].content[0].text` instead of `choices[0].message.content`).
"""

import sys

from _per_api_fault_helpers import ApiTestPlan, REPO_ROOT, parse_args_and_run


CASES = [
    # Sitting-5 Workstream A: assert on body discriminator (OpenAI Responses uses the same envelope as API1).
    {"name": "error_billing",      "fixture": "error_billing.json",      "expected_run_state": "failed",    "expected_log_substring": "insufficient_quota",    "expected_http_status_in_meta": 429},
    {"name": "error_throttle",     "fixture": "error_throttle.json",     "expected_run_state": "failed",    "expected_log_substring": "rate_limit_error",      "expected_http_status_in_meta": 429},
    {"name": "error_auth",         "fixture": "error_auth.json",         "expected_run_state": "failed",    "expected_log_substring": "authentication_error", "expected_http_status_in_meta": 401},
    {"name": "error_overload",     "fixture": "error_overload.json",     "expected_run_state": "failed",    "expected_log_substring": "server_error",          "expected_http_status_in_meta": 503},
    {"name": "malformed_utf8",     "fixture": "malformed_utf8.json",     "expected_run_state": "succeeded", "expected_log_substring": None,                    "expected_http_status_in_meta": None},
    {"name": "truncated_response", "fixture": "truncated_response.json", "expected_run_state": "failed",    "expected_log_substring": None,                    "expected_http_status_in_meta": None},
]


PLAN = ApiTestPlan(
    api_type="API2",
    fixture_dir=REPO_ROOT / "test" / "dispatch" / "fixtures" / "api2",
    cases=CASES,
    interface_prefix="mock_api2_fault_",
)


if __name__ == "__main__":
    sys.exit(parse_args_and_run(PLAN, "api2"))
