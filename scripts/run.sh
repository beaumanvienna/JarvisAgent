#!/usr/bin/env bash
# run.sh — run an executable located under workflows/
# Usage: run.sh <path-relative-to-workflows/>
# Example: run.sh exampleMakefile4/01_taskName/hello
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
