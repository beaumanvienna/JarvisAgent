#!/usr/bin/env bash
set -euo pipefail

TOOL_NAME="zipTool"

if [ "$#" -lt 2 ]; then
    echo "[$TOOL_NAME] Usage: zipTool archive_name file1 [file2 ...]" >&2
    echo "[$TOOL_NAME] Example: zipTool out.zip file1 file2 file3" >&2
    exit 2
fi

archive="$1"
shift

# Print an escaped view of the command so logs are unambiguous even with spaces.
{
    printf "[%s] Creating: %s from" "$TOOL_NAME" "$archive"
    for arg in "$@"; do
        printf " %q" "$arg"
    done
    printf "\n"
} >&2

if command -v zip >/dev/null 2>&1; then
    zip "$archive" "$@"
    exitCode=$?
else
    echo "[$TOOL_NAME] 'zip' not found, using Python zipfile fallback" >&2
    # Prefer 'python', fall back to 'python3'
    PYTHON_CMD="python"
    if ! command -v "$PYTHON_CMD" >/dev/null 2>&1; then
        PYTHON_CMD="python3"
    fi
    "$PYTHON_CMD" -c "
import sys, zipfile, os
archive = sys.argv[1]
files = sys.argv[2:]
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zf:
    for f in files:
        zf.write(f, os.path.basename(f))
        print(f'  adding: {os.path.basename(f)}', file=sys.stderr)
" "$archive" "$@"
    exitCode=$?
fi

echo "[$TOOL_NAME] Done (exit ${exitCode})" >&2
exit "${exitCode}"
