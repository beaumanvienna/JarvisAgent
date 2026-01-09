#!/usr/bin/env bash
set -euo pipefail

OBJ1="$1"
OBJ2="$2"
ARCHIVE="$3"

echo "[archive] $OBJ1 $OBJ2 -> $ARCHIVE"

# Create or replace archive
ar rcs "$ARCHIVE" "$OBJ1" "$OBJ2"
