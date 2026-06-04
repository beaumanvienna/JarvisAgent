#!/usr/bin/env python3
"""
Contract test: doc/jcwf.schema.json covers every field the JCWF parser reads.

The schema is the source of truth for what JCWF authors (and the generator AI)
see; if the parser reads a field that's not in the schema, authors have no way
to know it's valid, and the validator will reject otherwise-legitimate JCWFs.

This test greps `code/backend/application/workflow/workflowJsonParser.cpp` and
`workflowJsonParserDetails.cpp` for every `key == "..."` comparison, then walks
`doc/jcwf.schema.json` to confirm each name appears somewhere — either as a
property name, an enum value, or a $defs target.

Runs offline — no server required.
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "doc" / "jcwf.schema.json"
PARSER_PATHS = [
    REPO_ROOT / "code" / "backend" / "application" / "workflow" / "workflowJsonParser.cpp",
    REPO_ROOT / "code" / "backend" / "application" / "workflow" / "workflowJsonParserDetails.cpp",
]

# Fields read via `key == "..."` that aren't user-facing JCWF names.  These
# are internal nesting keys whose presence in the schema would be misleading
# (e.g. cron/webhook trigger params parsed dynamically from params.*).
PARSER_NONFIELD_ALLOWLIST: set[str] = set()
# (Filter-source keys already covered via $defs.filter.source.properties
#  or trigger-params parsed in workflowTriggerBinder, not the core schema.)


def extract_parser_field_names() -> set[str]:
    pattern = re.compile(r'key\s*==\s*"([a-z_]+)"')
    names: set[str] = set()
    for path in PARSER_PATHS:
        text = path.read_text(encoding="utf-8")
        names.update(pattern.findall(text))
    return names - PARSER_NONFIELD_ALLOWLIST


def collect_schema_tokens(node, out: set[str]) -> None:
    """Walks the schema and collects every property name, enum value, and $defs key."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "properties" and isinstance(value, dict):
                out.update(value.keys())
                for v in value.values():
                    collect_schema_tokens(v, out)
            elif key == "$defs" and isinstance(value, dict):
                out.update(value.keys())
                for v in value.values():
                    collect_schema_tokens(v, out)
            elif key == "enum" and isinstance(value, list):
                out.update(str(v) for v in value if isinstance(v, str))
            elif key == "required" and isinstance(value, list):
                out.update(str(v) for v in value if isinstance(v, str))
            else:
                collect_schema_tokens(value, out)
    elif isinstance(node, list):
        for item in node:
            collect_schema_tokens(item, out)


def main() -> int:
    if not SCHEMA_PATH.exists():
        print(f"FAIL: schema not found at {SCHEMA_PATH}")
        return 1
    for parser_path in PARSER_PATHS:
        if not parser_path.exists():
            print(f"FAIL: parser source not found at {parser_path}")
            return 1

    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    schema_tokens: set[str] = set()
    collect_schema_tokens(schema, schema_tokens)

    parser_fields = extract_parser_field_names()
    missing = sorted(f for f in parser_fields if f not in schema_tokens)

    if missing:
        print(f"FAIL: parser reads {len(missing)} field(s) not declared in doc/jcwf.schema.json:")
        for name in missing:
            print(f"  - {name}")
        print(
            "\nFix: add these fields to doc/jcwf.schema.json (either as properties "
            "on the relevant $defs entry, or as enum values for trigger/task types)."
        )
        return 1

    print(f"OK: all {len(parser_fields)} parser-read fields are declared in the schema.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
