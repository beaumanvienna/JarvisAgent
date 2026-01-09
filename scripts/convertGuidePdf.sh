#!/usr/bin/env bash
set -euo pipefail

echo "[convertGuidePdf.sh debug v3] argv_count=$#"
arg_index=0
for arg in "$@"
do
    echo "[convertGuidePdf.sh debug v3] argv[$arg_index]=$arg"
    arg_index=$((arg_index + 1))
done

if [ "$#" -lt 2 ]
then
    echo "[convertGuidePdf.sh debug v3] Usage: convertGuidePdf.sh <input.md> <output.pdf>"
    exit 2
fi

input_md_path="$1"
shift
output_pdf_path="$*"

echo "[convertGuidePdf.sh debug v3] input_md_path=$input_md_path"
echo "[convertGuidePdf.sh debug v3] output_pdf_path=$output_pdf_path"

input_abs="$(python3 - "$input_md_path" <<'PY'
import os
import sys
print(os.path.abspath(sys.argv[1]))
PY
)"

output_abs="$(python3 - "$output_pdf_path" <<'PY'
import os
import sys
print(os.path.abspath(sys.argv[1]))
PY
)"

echo "[convertGuidePdf.sh debug v3] input_abs=$input_abs"
echo "[convertGuidePdf.sh debug v3] output_abs=$output_abs"

output_dir="$(dirname "$output_abs")"
mkdir -p "$output_dir"

echo "[convertGuidePdf.sh debug v3] output_dir=$output_dir"
echo "[convertGuidePdf.sh debug v3] running: md2pdf "$input_abs" -o "$output_abs""

md2pdf "$input_abs" -o "$output_abs"

echo "[convertGuidePdf.sh debug v3] md2pdf exit_code=$?"
echo "[convertGuidePdf.sh debug v3] done"
