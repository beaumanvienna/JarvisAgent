#!/usr/bin/env bash
set -euo pipefail

TOOL_NAME="zipTool"

if ! command -v zip >/dev/null 2>&1; then
    echo "[$TOOL_NAME] Error: 'zip' is not installed or not on PATH." >&2
    echo "[$TOOL_NAME] Install on Ubuntu with: sudo apt install zip" >&2
    exit 127
fi

if [ "$#" -lt 1 ]; then
    echo "[$TOOL_NAME] Usage: zipTool [zip-options] archive_name [file ...]" >&2
    echo "[$TOOL_NAME] Example: zipTool out.zip file1 file2 file3" >&2
    exit 2
fi

# Print an escaped view of the command so logs are unambiguous even with spaces.
{
    printf "[%s] Running: zip" "$TOOL_NAME"
    for arg in "$@"; do
        printf " %q" "$arg"
    done
    printf "\n"
} >&2

zip "$@"
exitCode=$?

echo "[$TOOL_NAME] Done (exit ${exitCode})" >&2
exit "${exitCode}"
