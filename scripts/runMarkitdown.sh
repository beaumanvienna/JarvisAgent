#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: runMarkitdown.sh <input.pdf> <output.md>" >&2
    exit 2
fi

input_file="$1"
output_file="$2"

mkdir -p "$(dirname "$output_file")"

echo "[runMarkitdown.sh] Converting '$input_file' -> '$output_file'"
markitdown "$input_file" > "$output_file"
echo "[runMarkitdown.sh] Done ($(wc -c < "$output_file") bytes)"
