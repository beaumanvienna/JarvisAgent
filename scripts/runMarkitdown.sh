#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: runMarkitdown.sh <input.pdf> <output.md>" >&2
    exit 2
fi

input_file="$1"
output_file="$2"

# Heartbeat helper — resets the inactivity watchdog timer.
# JARVIS_PORT and JARVIS_TASK_ID are set by the runtime for shell tasks.
heartbeat() {
    if [ -n "${JARVIS_PORT:-}" ] && [ -n "${JARVIS_TASK_ID:-}" ]; then
        curl -s -X POST "http://localhost:${JARVIS_PORT}/api/task/${JARVIS_TASK_ID}/heartbeat" > /dev/null 2>&1 || true
    fi
}

mkdir -p "$(dirname "$output_file")"

# Ensure UTF-8 output on Windows (default cp1252 can't encode all characters).
export PYTHONIOENCODING=utf-8

echo "[runMarkitdown.sh] Converting '$input_file' -> '$output_file'"
heartbeat
markitdown "$input_file" > "$output_file"
heartbeat
echo "[runMarkitdown.sh] Done ($(wc -c < "$output_file") bytes)"
