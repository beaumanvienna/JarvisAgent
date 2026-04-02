#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

# Detect edition: --engine flag or script name containing "-engine"
EDITION="studio"
PASSTHROUGH_ARGS=()
for arg in "$@"; do
    if [[ "$arg" == "--engine" ]]; then
        EDITION="engine"
    else
        PASSTHROUGH_ARGS+=("$arg")
    fi
done
SCRIPT_NAME="$(basename "$0")"
if [[ "$SCRIPT_NAME" == *"-engine"* ]]; then
    EDITION="engine"
fi

if [[ "$EDITION" == "engine" ]]; then
    BINARY="$SCRIPT_DIR/bin/Release/jarvisAgent-engine"
    EDITION_LABEL="Engine"
else
    BINARY="$SCRIPT_DIR/bin/Release/jarvisAgent-studio"
    EDITION_LABEL="Studio"
fi
set -- "${PASSTHROUGH_ARGS[@]}"

# ── Check release binary exists ──────────────────────────────────────────────
if [ ! -f "$BINARY" ]; then
    echo "[jarvisagent.sh] ERROR: $EDITION_LABEL binary not found at $BINARY"
    echo "  JarvisAgent must be compiled first. See README.md for build instructions."
    exit 1
fi

# ── Create venv if missing ───────────────────────────────────────────────────
if [ ! -d "$VENV_DIR" ]; then
    echo "[jarvisagent.sh] Creating Python virtual environment in $VENV_DIR ..."
    python3 -m venv "$VENV_DIR"
    echo "[jarvisagent.sh] Installing Python dependencies ..."
    "$VENV_DIR/bin/pip" install --upgrade pip
    "$VENV_DIR/bin/pip" install "markitdown[all]"
    echo "[jarvisagent.sh] Virtual environment ready."
    echo "[jarvisagent.sh] NOTE: PDF workflows (vehicleTroubleshootingGuide) also require:"
    echo "  mmdc:   npm install -g @mermaid-js/mermaid-cli@10.x"
    echo "  pandoc: apt install pandoc texlive-latex-base texlive-latex-extra"
fi

# ── Activate venv ────────────────────────────────────────────────────────────
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"

# ── Launch JarvisAgent ───────────────────────────────────────────────────────
cd "$SCRIPT_DIR"
exec "$BINARY" "$@"
