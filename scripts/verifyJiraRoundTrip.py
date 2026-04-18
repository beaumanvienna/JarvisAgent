# @jarvis-script
# @short: Verify the Jira round-trip demo: create → get → field-match assertions
# @description: Reads response.json from the create_issue and get_issue tasks
#   of the jiraIssueDemo workflow and asserts that the retrieved issue matches
#   the payload that was submitted. Walks Atlassian Document Format trees to
#   compare description text, and reports any mismatches or missing fields as
#   a structured result.
# @outputs: verify_result — dict with success flag and per-field mismatch details
"""Verify the Jira round-trip demo: create → get → field-match assertions.

Called as a python task after get_issue.  Reads the response.json files written
by the create_issue and get_issue tasks and asserts the retrieved issue matches
what was submitted.
"""

import json
import os


def _walk_adf_text(node):
    """Recursively collect all text nodes from an Atlassian Document Format tree."""
    if isinstance(node, dict):
        if node.get("type") == "text" and isinstance(node.get("text"), str):
            yield node["text"]
        for child in node.get("content", []) or []:
            yield from _walk_adf_text(child)
    elif isinstance(node, list):
        for child in node:
            yield from _walk_adf_text(child)


def verify(context=None, **kwargs) -> dict:
    if context is None:
        return {"error": "no context provided"}

    workflow_base = context.get("_workflow_base_directory", "")
    if not workflow_base:
        return {"error": "no _workflow_base_directory in context"}

    create_path = os.path.join(workflow_base, "jiraIssueDemo", "02_create", "response.json")
    get_path = os.path.join(workflow_base, "jiraIssueDemo", "03_get", "response.json")
    ai_report_path = os.path.join(
        workflow_base, "..", "queue", "jiraIssueDemo", "01_ai_report", "PROB_bug.output.txt"
    )

    for p, label in [(create_path, "create response"), (get_path, "get response")]:
        if not os.path.isfile(p):
            return {"error": f"{label} not found: {p}"}

    with open(create_path, "r") as f:
        create_resp = json.load(f)
    with open(get_path, "r") as f:
        get_resp = json.load(f)

    create_key = create_resp.get("key")
    get_key = get_resp.get("key")

    if not create_key:
        return {"error": f"create response missing 'key': {create_resp}"}

    if create_key != get_key:
        return {"error": f"key mismatch: created {create_key}, retrieved {get_key}"}

    fields = get_resp.get("fields", {}) or {}
    summary = fields.get("summary", "")
    issuetype = (fields.get("issuetype") or {}).get("name", "")
    labels = fields.get("labels", []) or []

    description_text = " ".join(_walk_adf_text(fields.get("description"))).strip()

    # Assertions
    errors = []
    if "OneDrive upload timeout" not in summary:
        errors.append(f"summary mismatch: {summary!r}")
    if issuetype != "Bug":
        errors.append(f"issuetype mismatch: {issuetype!r} (expected 'Bug')")
    if "automated" not in labels or "cloud-integration" not in labels:
        errors.append(f"labels missing expected values: {labels}")
    if not description_text:
        errors.append("description is empty — AI text did not round-trip")

    # Compare with source AI output if accessible
    if os.path.isfile(ai_report_path):
        with open(ai_report_path, "r") as f:
            ai_text = f.read().strip()
        # First 20 characters of the AI report should appear in the retrieved description
        snippet = ai_text[:20].replace("\n", " ").replace("\r", " ").strip()
        if snippet and snippet not in description_text:
            errors.append(
                f"AI text not found in retrieved description. snippet={snippet!r} "
                f"description={description_text[:120]!r}"
            )

    if errors:
        return {"ok": False, "errors": errors, "issue_key": create_key}

    print(f"Jira round-trip verified: {create_key} — summary, type, labels, description all match.")
    return {
        "ok": True,
        "issue_key": create_key,
        "summary": summary,
        "labels": labels,
        "description_len": len(description_text),
    }
