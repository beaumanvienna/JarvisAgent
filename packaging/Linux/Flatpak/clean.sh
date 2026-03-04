#!/bin/bash
# clean.sh — Remove Flatpak build artifacts.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$SCRIPT_DIR/build"
rm -rf "$SCRIPT_DIR/.flatpak-builder"
echo "Cleaned: $SCRIPT_DIR/build and $SCRIPT_DIR/.flatpak-builder"
