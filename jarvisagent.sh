#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
BINARY="$SCRIPT_DIR/bin/Release/jarvisAgent"

# ── Check release binary exists ──────────────────────────────────────────────
if [ ! -f "$BINARY" ]; then
    echo "[jarvisagent.sh] ERROR: Release binary not found at $BINARY"
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
