#!/usr/bin/env python3
"""
Per-fixture fault test for API4 (Anthropic Messages) through MockTransport.

Anthropic error envelope: {"type": "error", "error": {"type": "<discriminator>",
"message": "..."}}.  The discriminator is one level deeper than API1's.
ReplyParserAPI4 already extracts nested `error.type` and maps `rate_limit_error`
/ `authentication_error` / `permission_error` / `overloaded_error|api_error`
to HTTP statuses; missing today is `credit_balance_too_low` (Anthropic's
billing-exhausted variant returns HTTP 400, not 429).

Real Anthropic HTTP statuses:
  credit_balance_too_low → 400
  rate_limit_error       → 429
  authentication_error   → 401
  overloaded_error       → 529  (non-standard status)
"""

import sys

from _per_api_fault_helpers import ApiTestPlan, REPO_ROOT, parse_args_and_run


CASES = [
    # Sitting-5 Workstream A: Anthropic's nested error.type is what ReplyParserAPI4.GetError
    # surfaces as m_ProviderErrorType.  All four discriminators distinct (unlike Gemini's
    # collision on RESOURCE_EXHAUSTED), so categorization is fully deterministic by string match.
    {"name": "error_billing",      "fixture": "error_billing.json",      "expected_run_state": "failed",    "expected_log_substring": "credit_balance_too_low", "expected_http_status_in_meta": 400},
    {"name": "error_throttle",     "fixture": "error_throttle.json",     "expected_run_state": "failed",    "expected_log_substring": "rate_limit_error",       "expected_http_status_in_meta": 429},
    {"name": "error_auth",         "fixture": "error_auth.json",         "expected_run_state": "failed",    "expected_log_substring": "authentication_error",  "expected_http_status_in_meta": 401},
    {"name": "error_overload",     "fixture": "error_overload.json",     "expected_run_state": "failed",    "expected_log_substring": "overloaded_error",       "expected_http_status_in_meta": 529},
    {"name": "malformed_utf8",     "fixture": "malformed_utf8.json",     "expected_run_state": "succeeded", "expected_log_substring": None,                     "expected_http_status_in_meta": None},
    {"name": "truncated_response", "fixture": "truncated_response.json", "expected_run_state": "failed",    "expected_log_substring": None,                     "expected_http_status_in_meta": None},
]


PLAN = ApiTestPlan(
    api_type="API4",
    fixture_dir=REPO_ROOT / "test" / "dispatch" / "fixtures" / "api4",
    cases=CASES,
    interface_prefix="mock_api4_fault_",
)


if __name__ == "__main__":
    sys.exit(parse_args_and_run(PLAN, "api4"))
