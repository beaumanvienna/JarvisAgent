#!/usr/bin/env python3
"""
Per-fixture fault test for API3 (Google Gemini) through MockTransport.

Gemini error envelope: {"error": {"code": <int>, "message": "...", "status":
"<RESOURCE_EXHAUSTED|UNAUTHENTICATED|UNAVAILABLE|...>", "details": [...]}}.
The billing-vs-throttle distinction lives inside `error.details[*].reason`
(`USER_PROJECT_QUOTA_EXCEEDED` / `BILLING_DISABLED` vs `RATE_LIMIT_EXCEEDED`).
Sitting 6 (Workstream E) wired `details[*]` walking into ReplyParserAPI3 —
the first `reason` value surfaces as m_ProviderErrorCode in the WS payload
+ log line, and classification mapping picks the right ProviderErrorCategory.
"""

import sys

from _per_api_fault_helpers import ApiTestPlan, REPO_ROOT, parse_args_and_run


CASES = [
    # Sitting-6 Workstream E: Gemini's `error.details[*].reason` is now parsed and surfaced as
    # m_ProviderErrorCode in the OnRequestFailed log line, so billing vs throttle (both
    # RESOURCE_EXHAUSTED at the status level) become distinguishable via USER_PROJECT_QUOTA_EXCEEDED
    # vs RATE_LIMIT_EXCEEDED.  Auth + overload have unambiguous status enums.
    {"name": "error_billing",      "fixture": "error_billing.json",      "expected_run_state": "failed",    "expected_log_substring": "USER_PROJECT_QUOTA_EXCEEDED", "expected_http_status_in_meta": 429},
    {"name": "error_throttle",     "fixture": "error_throttle.json",     "expected_run_state": "failed",    "expected_log_substring": "RATE_LIMIT_EXCEEDED",         "expected_http_status_in_meta": 429},
    {"name": "error_auth",         "fixture": "error_auth.json",         "expected_run_state": "failed",    "expected_log_substring": "UNAUTHENTICATED",             "expected_http_status_in_meta": 401},
    {"name": "error_overload",     "fixture": "error_overload.json",     "expected_run_state": "failed",    "expected_log_substring": "UNAVAILABLE",                 "expected_http_status_in_meta": 503},
    {"name": "malformed_utf8",     "fixture": "malformed_utf8.json",     "expected_run_state": "succeeded", "expected_log_substring": None,                          "expected_http_status_in_meta": None},
    {"name": "truncated_response", "fixture": "truncated_response.json", "expected_run_state": "failed",    "expected_log_substring": None,                          "expected_http_status_in_meta": None},
]


PLAN = ApiTestPlan(
    api_type="API3",
    fixture_dir=REPO_ROOT / "test" / "dispatch" / "fixtures" / "api3",
    cases=CASES,
    interface_prefix="mock_api3_fault_",
)


if __name__ == "__main__":
    sys.exit(parse_args_and_run(PLAN, "api3"))
