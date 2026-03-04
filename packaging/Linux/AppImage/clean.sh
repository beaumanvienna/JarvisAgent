#!/bin/bash
# clean.sh — Remove AppImage build artifacts.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$SCRIPT_DIR/build"
echo "Cleaned: $SCRIPT_DIR/build"
