#!/usr/bin/env bash
# @jarvis-script
# @short: Convert a Markdown file to PDF using md2pdf
# @params: INPUT_MD — path to the input Markdown file
#   OUTPUT_PDF — path to the output PDF file
# @description: Converts Markdown to PDF using md2pdf with system Chrome.
#   Auto-detects Chrome location per platform.
# @outputs: OUTPUT_PDF — the generated PDF file
set -euo pipefail

# Portable Python detection: prefer 'python' (Windows), fall back to 'python3'.
PYTHON_CMD="python3"
if command -v python >/dev/null 2>&1; then
    PYTHON_CMD="python"
elif ! command -v python3 >/dev/null 2>&1; then
    echo "[convertGuidePdf.sh] ERROR: neither 'python' nor 'python3' found on PATH" >&2
    exit 127
fi

# Use system Chrome so Playwright does not need its own Chromium download.
# Auto-detect Chrome location per platform.
if [ -n "${CHROME_PATH:-}" ]; then
    : # already set by caller / environment
elif [ -f "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" ]; then
    export CHROME_PATH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
elif [ -f "/usr/bin/google-chrome" ]; then
    export CHROME_PATH="/usr/bin/google-chrome"
elif [ -f "/usr/bin/chromium-browser" ]; then
    export CHROME_PATH="/usr/bin/chromium-browser"
elif [ -f "/usr/bin/chromium" ]; then
    export CHROME_PATH="/usr/bin/chromium"
elif [ -f "C:/Program Files/Google/Chrome/Application/chrome.exe" ]; then
    export CHROME_PATH="C:/Program Files/Google/Chrome/Application/chrome.exe"
elif [ -f "C:/Program Files (x86)/Google/Chrome/Application/chrome.exe" ]; then
    export CHROME_PATH="C:/Program Files (x86)/Google/Chrome/Application/chrome.exe"
else
    echo "[convertGuidePdf.sh] WARNING: Chrome not found, md2pdf may fail" >&2
fi

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

input_abs="$($PYTHON_CMD - "$input_md_path" <<'PY'
import os
import sys
print(os.path.abspath(sys.argv[1]))
PY
)"

output_abs="$($PYTHON_CMD - "$output_pdf_path" <<'PY'
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
