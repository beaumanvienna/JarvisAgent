#!/usr/bin/env python3
"""
Per-fixture fault test for API5 (AWS Bedrock) through MockTransport.

Bedrock dispatches the underlying model family's body — success fixtures here
use the Anthropic-on-Bedrock shape (`content` field) so they delegate to
ReplyParserAPI4.  Error envelope is AWS-flavoured:
{"__type": "<Exception>", "message": "..."}.  Sitting 6 (Workstream E) added
a new BedrockFamily::AwsError variant — ReplyParserAPI5 peeks at `__type`
BEFORE family delegation, populates m_AwsErrorType + m_AwsErrorMessage
directly (no delegate), and GetError returns the classified AiError with
the AWS exception short name as m_ProviderErrorType.

Real Bedrock HTTP statuses:
  ServiceQuotaExceededException → 400  (BillingExhausted)
  ThrottlingException           → 429  (ThrottleRateLimit)
  AccessDeniedException         → 403  (AuthFailure)
  ModelStreamErrorException     → 503  (ServiceOverload)
"""

import sys

from _per_api_fault_helpers import ApiTestPlan, REPO_ROOT, parse_args_and_run


CASES = [
    # Sitting-6 Workstream E: AWS `__type` pre-parse landed.  ReplyParserAPI5 detects the
    # {"__type": "<Exception>", "message": "..."} shape BEFORE delegating to a model family
    # and surfaces the exception name as m_ProviderErrorType.  Drivers now assert on the AWS
    # exception name; classification covers billing / throttle / auth / overload variants.
    {"name": "error_billing",      "fixture": "error_billing.json",      "expected_run_state": "failed",    "expected_log_substring": "ServiceQuotaExceededException", "expected_http_status_in_meta": 400},
    {"name": "error_throttle",     "fixture": "error_throttle.json",     "expected_run_state": "failed",    "expected_log_substring": "ThrottlingException",           "expected_http_status_in_meta": 429},
    {"name": "error_auth",         "fixture": "error_auth.json",         "expected_run_state": "failed",    "expected_log_substring": "AccessDeniedException",         "expected_http_status_in_meta": 403},
    {"name": "error_overload",     "fixture": "error_overload.json",     "expected_run_state": "failed",    "expected_log_substring": "ModelStreamErrorException",     "expected_http_status_in_meta": 503},
    {"name": "malformed_utf8",     "fixture": "malformed_utf8.json",     "expected_run_state": "succeeded", "expected_log_substring": None,                            "expected_http_status_in_meta": None},
    {"name": "truncated_response", "fixture": "truncated_response.json", "expected_run_state": "failed",    "expected_log_substring": None,                            "expected_http_status_in_meta": None},
]


PLAN = ApiTestPlan(
    api_type="API5",
    fixture_dir=REPO_ROOT / "test" / "dispatch" / "fixtures" / "api5",
    cases=CASES,
    interface_prefix="mock_api5_fault_",
)


if __name__ == "__main__":
    sys.exit(parse_args_and_run(PLAN, "api5"))
