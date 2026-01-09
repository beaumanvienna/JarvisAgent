#!/usr/bin/env bash
set -euo pipefail

# Files that are essential for debugging executors + registry +
# orchestrator without blowing up the output.
INCLUDE_PATTERNS=(
    "../queue/vehicleTroubleshootingGuide/01_aiCode244/*"
)

WORKFLOW_DIR="."

for pattern in "${INCLUDE_PATTERNS[@]}"; do
    for f in "$WORKFLOW_DIR"/$pattern; do
        # Skip if no match
        [[ -e "$f" ]] || continue

        echo "=== $(basename "$f") ==="
        cat "$f"
        echo ""
    done
done

echo "=== Done ==="
