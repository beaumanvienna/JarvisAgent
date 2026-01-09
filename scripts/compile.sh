#!/usr/bin/env bash
set -euo pipefail

SOURCE="$1"
OUTPUT="$2"

echo "[compile] $SOURCE -> $OUTPUT"

g++ -Wall -Wextra -std=c++20 -c "$SOURCE" -o "$OUTPUT" "${@:3}"

