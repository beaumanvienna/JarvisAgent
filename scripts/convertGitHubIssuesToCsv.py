# @jarvis-script
# @short: Convert a GitHub issues JSON response into a CSV for filter tasks
# @params: input_json — path to the GitHub API /issues response (file_inputs[0])
# @description: Reads the GitHub API issues response, drops pull requests
#   (GitHub returns PRs in the /issues endpoint), and writes issues.csv with
#   number / title / body / labels columns into the task working directory.
#   Body text is truncated to 500 characters with newlines collapsed so the CSV
#   stays parseable by downstream filter tasks.
# @outputs: issues.csv — one row per open or closed issue (PRs excluded)
"""Convert GitHub issues JSON to CSV for filter consumption.

Called as a python task with file_inputs[0] = response.json, file_outputs[0] = issues.csv.
The executor injects _file_input_0 and _file_output_0 into the context dict.
"""

import csv
import json
import os


def convert(context=None, **kwargs) -> dict:
    """Read GitHub issues JSON, write CSV with number/title/body/labels columns."""
    if context is None:
        return {"error": "no context provided"}

    input_path = context.get("_file_input_0", "")
    # file_outputs are resolved via the task working directory
    output_dir = context.get("_task_working_directory", ".")
    output_path = os.path.join(output_dir, "issues.csv")

    if not input_path or not os.path.isfile(input_path):
        return {"error": f"input file not found: {input_path}"}

    with open(input_path, "r") as f:
        data = json.load(f)

    # Filter out pull requests (GitHub API returns PRs in the issues endpoint)
    issues = [i for i in data if "pull_request" not in i]

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

    with open(output_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["number", "title", "body", "labels"])
        writer.writeheader()
        for issue in issues:
            body = (issue.get("body") or "")[:500].replace("\n", " ").replace("\r", "")
            labels = ",".join(label["name"] for label in issue.get("labels", []))
            writer.writerow(
                {
                    "number": issue["number"],
                    "title": issue["title"],
                    "body": body,
                    "labels": labels,
                }
            )

    print(f"Converted {len(issues)} issues to {output_path}")
    return {"count": len(issues)}
