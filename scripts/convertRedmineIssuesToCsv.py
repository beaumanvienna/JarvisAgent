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
