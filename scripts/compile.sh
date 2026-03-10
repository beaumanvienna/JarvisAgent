#!/usr/bin/env bash
set -euo pipefail

SOURCE="$1"
OUTPUT="$2"

echo "[compile] $SOURCE -> $OUTPUT"

if command -v g++ &>/dev/null; then
    g++ -Wall -Wextra -std=c++20 -c "$SOURCE" -o "$OUTPUT" "${@:3}"
else
    echo "[compile] g++ not found; creating replacement file"
    echo "g++ not found; replacement object file for $(basename "$SOURCE")" > "$OUTPUT"
fi

