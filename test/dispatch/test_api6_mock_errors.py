#!/usr/bin/env python3
"""
Per-fixture fault test for API6 (Azure OpenAI) through MockTransport.

Azure OpenAI's response envelope is byte-identical to OpenAI Chat (API1) —
configParser routes API6 to `ReplyParserAPI1` directly (per
`application/json/replyParser.cpp:88`).  This driver validates that routing
is intact: the same battery as api1, dispatched through `api_type: "API6"`,
must produce identical end-to-end behaviour.  Parity-validation step, not
new-parser-coverage.
"""

import sys

from _per_api_fault_helpers import ApiTestPlan, REPO_ROOT, parse_args_and_run


CASES = [
    # Sitting-5 Workstream A: Azure OpenAI inherits OpenAI's body discriminators via the API1 parser.
    {"name": "error_billing",      "fixture": "error_billing.json",      "expected_run_state": "failed",    "expected_log_substring": "insufficient_quota",    "expected_http_status_in_meta": 429},
    {"name": "error_throttle",     "fixture": "error_throttle.json",     "expected_run_state": "failed",    "expected_log_substring": "rate_limit_error",      "expected_http_status_in_meta": 429},
    {"name": "error_auth",         "fixture": "error_auth.json",         "expected_run_state": "failed",    "expected_log_substring": "authentication_error", "expected_http_status_in_meta": 401},
    {"name": "error_overload",     "fixture": "error_overload.json",     "expected_run_state": "failed",    "expected_log_substring": "server_error",          "expected_http_status_in_meta": 503},
    {"name": "malformed_utf8",     "fixture": "malformed_utf8.json",     "expected_run_state": "succeeded", "expected_log_substring": None,                    "expected_http_status_in_meta": None},
    {"name": "truncated_response", "fixture": "truncated_response.json", "expected_run_state": "failed",    "expected_log_substring": None,                    "expected_http_status_in_meta": None},
]


PLAN = ApiTestPlan(
    api_type="API6",
    fixture_dir=REPO_ROOT / "test" / "dispatch" / "fixtures" / "api6",
    cases=CASES,
    interface_prefix="mock_api6_fault_",
)


if __name__ == "__main__":
    sys.exit(parse_args_and_run(PLAN, "api6"))
