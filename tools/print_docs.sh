#!/usr/bin/env bash
set -euo pipefail

# -------------------------------------------------------------------
# Resolve script directory → absolute path
# -------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# -------------------------------------------------------------------
# Find all markdown docs (exclude vendor/)
# -------------------------------------------------------------------
find "$REPO_ROOT" \
  -type f \
  -name "*.md" \
  -not -path "$REPO_ROOT/vendor/*" \
  | sort \
  | while read -r file; do
      echo "------------------------------------------------------------"
      echo "## FILE: $file"
      echo "------------------------------------------------------------"
      cat "$file"
      echo -e "\n\n"
    done
