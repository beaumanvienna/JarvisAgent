#!/usr/bin/env bash
# @jarvis-script
# @short: Run an executable located under workflows/
# @params: PATH — path relative to workflows/ directory
# @description: Locates and executes a binary under workflows/. Validates that
#   the target exists and is executable before running it.
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "Usage: run.sh <path-relative-to-workflows/>" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LAUNCH_DIR="$(dirname "$SCRIPT_DIR")"
TARGET="${LAUNCH_DIR}/workflows/$1"
shift

if [ ! -f "$TARGET" ]; then
  echo "Error: '${TARGET}' not found." >&2
  exit 1
fi

if [ ! -x "$TARGET" ]; then
  echo "Error: '${TARGET}' is not executable." >&2
  exit 1
fi

exec "$TARGET" "$@"
