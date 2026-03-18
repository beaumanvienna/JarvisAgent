#!/usr/bin/env bash
# @jarvis-script
# @short: Print labelled input values for workflow debugging
# @params: LABEL — descriptive label for the output
#   VALUES... — one or more values to print
# @description: Prints its arguments so the workflow log captures them.
#   Used by the inputResolutionTest workflow to verify resolved input values.

if [ $# -lt 1 ]; then
    echo "[echoInput.sh] ERROR: expected at least 1 argument" >&2
    exit 1
fi

label="$1"
shift

echo "[echoInput.sh] $label = $*"
