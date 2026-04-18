# @jarvis-script
# @short: Convert a Redmine issues JSON response into a CSV for filter tasks
# @params: input_json — path to the Redmine /issues.json response (file_inputs[0])
# @description: Reads a Redmine issues response payload and writes issues.csv
#   with id / subject / description / tracker columns into the task working
#   directory. Description text is truncated to 500 characters and stripped of
#   newlines so the output is safe to consume from a csv filter task.
# @outputs: issues.csv — one row per Redmine issue
"""Convert Redmine issues JSON to CSV for filter consumption.

Called as a python task with file_inputs[0] = response.json, file_outputs[0] = issues.csv.
The executor injects _file_input_0 and _file_output_0 into the context dict.
"""

import csv
import json
import os


def convert(context=None, **kwargs) -> dict:
    """Read Redmine issues JSON, write CSV with id/subject/description/tracker columns."""
    if context is None:
        return {"error": "no context provided"}

    input_path = context.get("_file_input_0", "")
    output_dir = context.get("_task_working_directory", ".")
    output_path = os.path.join(output_dir, "issues.csv")

    if not input_path or not os.path.isfile(input_path):
        return {"error": f"input file not found: {input_path}"}

    with open(input_path, "r") as f:
        data = json.load(f)

    issues = data.get("issues", [])

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)

    with open(output_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["id", "subject", "description", "tracker"])
        writer.writeheader()
        for issue in issues:
            description = (issue.get("description") or "")[:1000].replace("\n", " ").replace("\r", "")
            writer.writerow(
                {
                    "id": issue["id"],
                    "subject": issue.get("subject", ""),
                    "description": description,
                    "tracker": (issue.get("tracker") or {}).get("name", ""),
                }
            )

    print(f"Converted {len(issues)} Redmine issues to {output_path}")
    return {"count": len(issues)}
