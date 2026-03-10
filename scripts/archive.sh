#!/usr/bin/env bash
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
