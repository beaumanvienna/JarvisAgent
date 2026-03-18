#!/usr/bin/env bash
# @jarvis-script
# @short: Compile a single C++ source file with g++
# @params: SOURCE — path to the .cpp source file
#   OUTPUT — path to the .o object file
# @description: Compiles a C++ source file using g++ with -Wall -Wextra -std=c++20.
#   Falls back to creating a placeholder file if g++ is not installed.
# @outputs: OUTPUT — the compiled object file
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

