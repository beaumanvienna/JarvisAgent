#!/usr/bin/env bash
# @jarvis-script
# @short: Create a static library archive from object files
# @params: OBJ1 — first object file path
#   OBJ2 — second object file path
#   ARCHIVE — output archive path
# @description: Creates a static library using 'ar rcs'. Falls back to a
#   placeholder file if ar is not installed.
# @outputs: ARCHIVE — the created .a archive file
set -euo pipefail

OBJ1="$1"
OBJ2="$2"
ARCHIVE="$3"

echo "[archive] $OBJ1 $OBJ2 -> $ARCHIVE"

if command -v ar &>/dev/null; then
    # Create or replace archive
    ar rcs "$ARCHIVE" "$OBJ1" "$OBJ2"
else
    echo "[archive] ar not found; creating replacement file"
    echo "ar not found; replacement archive containing $(basename "$OBJ1") $(basename "$OBJ2")" > "$ARCHIVE"
fi
