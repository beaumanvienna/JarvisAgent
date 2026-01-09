#!/usr/bin/env bash
set -euo pipefail

MAIN_OBJ="$1"
APP_OBJ="$2"
ARCHIVE="$3"
OUTPUT="$4"

echo "[link] $MAIN_OBJ $APP_OBJ $ARCHIVE -> $OUTPUT"

g++ -Wall -Wextra "$MAIN_OBJ" "$APP_OBJ" "$ARCHIVE" -o "$OUTPUT" "${@:5}"

