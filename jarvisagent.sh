#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

# Detect edition: --engine or --studio flag, or script name containing "-engine".
# Detect build config: --debug selects bin/Debug/ (with debug_signals endpoint enabled),
# otherwise the default bin/Release/ binary is used.
EDITION="studio"
BUILD_CONFIG="Release"
PASSTHROUGH_ARGS=()
for arg in "$@"; do
    if [[ "$arg" == "--engine" ]]; then
        EDITION="engine"
    elif [[ "$arg" == "--studio" ]]; then
        EDITION="studio"
    elif [[ "$arg" == "--debug" ]]; then
        BUILD_CONFIG="Debug"
    else
        PASSTHROUGH_ARGS+=("$arg")
    fi
done
SCRIPT_NAME="$(basename "$0")"
if [[ "$SCRIPT_NAME" == *"-engine"* ]]; then
    EDITION="engine"
fi

if [[ "$EDITION" == "engine" ]]; then
    BINARY="$SCRIPT_DIR/bin/$BUILD_CONFIG/jarvisAgent-engine"
    EDITION_LABEL="Engine"
else
    BINARY="$SCRIPT_DIR/bin/$BUILD_CONFIG/jarvisAgent-studio"
    EDITION_LABEL="Studio"
fi
if [[ "$BUILD_CONFIG" == "Debug" ]]; then
    EDITION_LABEL="$EDITION_LABEL (debug)"
fi
set -- "${PASSTHROUGH_ARGS[@]}"

# ── Check binary exists ──────────────────────────────────────────────────────
if [ ! -f "$BINARY" ]; then
    echo "[jarvisagent.sh] ERROR: $EDITION_LABEL binary not found at $BINARY"
    if [[ "$BUILD_CONFIG" == "Debug" ]]; then
        echo "  Build it with:  make config=debug"
        echo "  (Release binaries live in bin/Release/; --debug selects bin/Debug/.)"
    else
        echo "  JarvisAgent must be compiled first. See README.md for build instructions."
    fi
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
fi

# ── Install mmdc (mermaid-cli) if missing ───────────────────────────────────
if ! command -v mmdc &>/dev/null; then
    echo "[jarvisagent.sh] Installing mmdc (mermaid-cli) for PDF workflows ..."
    npm install --prefix "$SCRIPT_DIR/.npm-tools" @mermaid-js/mermaid-cli@10.x 2>/dev/null \
        || echo "[jarvisagent.sh] WARNING: mmdc install failed (npm required)"
fi

# Add local npm-tools bin to PATH (mmdc lives here)
if [[ -d "$SCRIPT_DIR/.npm-tools/node_modules/.bin" ]]; then
    export PATH="$SCRIPT_DIR/.npm-tools/node_modules/.bin:$PATH"
fi

# ── Hint about pandoc if missing ────────────────────────────────────────────
if ! command -v pandoc &>/dev/null; then
    echo ""
    echo "[jarvisagent.sh] WARNING: 'pandoc' is not installed."
    echo "  PDF workflows will fail without it. Please install pandoc and a LaTeX distribution:"
    echo ""
    echo "  Ubuntu/Debian:  sudo apt install pandoc texlive-latex-base texlive-latex-extra texlive-fonts-recommended"
    echo "  Fedora/RHEL:    sudo dnf install pandoc texlive-latex"
    echo "  Arch:           sudo pacman -S pandoc texlive-bin"
    echo "  macOS:          brew install pandoc basictex"
    echo ""
fi

# ── Activate venv ────────────────────────────────────────────────────────────
# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"

# ── Launch JarvisAgent ───────────────────────────────────────────────────────
cd "$SCRIPT_DIR"
exec "$BINARY" "$@"
