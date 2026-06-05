#!/usr/bin/env bash
# jarvisagent-launcher.sh — User-space launcher for system-wide JarvisAgent installs.
#
# Installed to /usr/bin/jarvisagent by deb/rpm/Arch packages.
# Symlinks: jarvisagent-studio → jarvisagent, j9t → jarvisagent
# Engine:   jarvisagent-engine (separate copy of this script, or symlink detected by name)
#
# Creates a per-user working directory with symlinks to read-only assets
# in /opt/jarvisagent/ and writable directories for user data.
#
# Usage:
#   jarvisagent                          # Studio edition (default: ~/JarvisAgent)
#   jarvisagent-engine                   # Engine edition
#   jarvisagent --home /path/to/dir      # custom working directory
#   JARVISAGENT_HOME=/tmp/ja jarvisagent # via environment variable
#   jarvisagent --help                   # pass-through to binary
#   jarvisagent --version                # pass-through to binary
#   jarvisagent --no-browser             # skip opening dashboard in browser

set -euo pipefail

INSTALL_DIR="/opt/jarvisagent"
OPEN_BROWSER=true
USER_HOME=""

# Detect edition from script name (jarvisagent-engine → Engine, everything else → Studio)
SCRIPT_NAME="$(basename "$0")"
if [[ "$SCRIPT_NAME" == *"-engine"* ]]; then
    BINARY_NAME="jarvisAgent-engine"
    EDITION_LABEL="Engine"
else
    BINARY_NAME="jarvisAgent-studio"
    EDITION_LABEL="Studio"
fi

# ---- Parse launcher-specific arguments ----
PASSTHROUGH_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --home)
            if [[ -z "${2:-}" ]]; then
                echo "Error: --home requires a directory argument"
                exit 1
            fi
            USER_HOME="$2"
            shift 2
            ;;
        --no-browser)
            OPEN_BROWSER=false
            shift
            ;;
        --help|-h|--version|-v)
            exec "$INSTALL_DIR/bin/$BINARY_NAME" "$1"
            ;;
        *)
            PASSTHROUGH_ARGS+=("$1")
            shift
            ;;
    esac
done

# ---- Determine working directory ----
if [[ -z "$USER_HOME" ]]; then
    USER_HOME="${JARVISAGENT_HOME:-$HOME/JarvisAgent}"
fi

# ---- Verify install directory ----
if [[ ! -d "$INSTALL_DIR" ]]; then
    echo "Error: $INSTALL_DIR not found."
    echo "Please install JarvisAgent first (e.g. sudo dnf install jarvisagent)."
    exit 1
fi

if [[ ! -x "$INSTALL_DIR/bin/$BINARY_NAME" ]]; then
    echo "Error: $INSTALL_DIR/bin/$BINARY_NAME not found or not executable."
    exit 1
fi

# ---- First-run setup ----
if [[ ! -d "$USER_HOME" ]]; then
    echo "==> First run: creating working directory at $USER_HOME"
    mkdir -p "$USER_HOME"
fi

# Symlink read-only assets (re-created every launch to pick up package updates)
# Uses ln -sfn to atomically replace existing symlinks without rm.
# If a real directory already exists (e.g. git clone), skip it — content is there.
# NOTE: scripts/ is deliberately NOT symlinked — see the copy step below.
for asset in bin dashboard workflow-editor doc; do
    if [[ -d "$INSTALL_DIR/$asset" ]]; then
        if [[ -L "$USER_HOME/$asset" ]] || [[ ! -e "$USER_HOME/$asset" ]]; then
            ln -sfn "$INSTALL_DIR/$asset" "$USER_HOME/$asset"
        fi
    fi
done

# Ensure binaries are reachable even if bin/ is a real directory
# (e.g. git clone has bin/Release/ but no bin/jarvisAgent)
if [[ -d "$USER_HOME/bin" && ! -L "$USER_HOME/bin" ]]; then
    for bin in jarvisAgent-studio jarvisAgent-engine; do
        if [[ -f "$INSTALL_DIR/bin/$bin" && ! -e "$USER_HOME/bin/$bin" ]]; then
            ln -sfn "$INSTALL_DIR/bin/$bin" "$USER_HOME/bin/$bin"
        fi
    done
fi

# If dashboard/workflow-editor dist/ folders are real directories, offer to
# replace them with symlinks to the installed (packaged) versions.
for ui_dist in dashboard/ui/dist workflow-editor/ui/dist; do
    installed="$INSTALL_DIR/$ui_dist"
    local_dir="$USER_HOME/$ui_dist"
    top_dir="$USER_HOME/${ui_dist%%/*}"
    # Only prompt if the top-level dir is a real directory (not a symlink)
    if [[ -d "$top_dir" && ! -L "$top_dir" && -d "$installed" && -d "$local_dir" && ! -L "$local_dir" ]]; then
        read -rp "==> $ui_dist already exists. Use installed version? [y/N] " answer
        if [[ "$answer" =~ ^[Yy]$ ]]; then
            mv "$local_dir" "${local_dir}.bak"
            ln -sfn "$installed" "$local_dir"
            echo "    Renamed to $(basename "$ui_dist").bak, using installed version."
        fi
    fi
done

# Create writable directories if they don't exist
mkdir -p "$USER_HOME/queue"
mkdir -p "$USER_HOME/log"
mkdir -p "$USER_HOME/workflows"

# Copy example workflows on first run (only if workflows dir is empty)
if [[ -d "$INSTALL_DIR/workflows" ]] && [[ -z "$(ls -A "$USER_HOME/workflows" 2>/dev/null)" ]]; then
    cp -a "$INSTALL_DIR/workflows/"* "$USER_HOME/workflows/" 2>/dev/null || true
    echo "==> Copied example workflows to $USER_HOME/workflows/"
fi

# Copy bundled scripts as REAL files (not a symlink). Workflow shell/python tasks
# reference scripts/ via path-confined relative paths; confinement canonicalises
# the path (follows symlinks) and rejects one that resolves outside the working
# directory — so a scripts/ symlink to the install dir breaks every script task.
if [[ -L "$USER_HOME/scripts" ]]; then rm -f "$USER_HOME/scripts"; fi  # drop a stale symlink from an older install
if [[ -d "$INSTALL_DIR/scripts" ]] && [[ ! -d "$USER_HOME/scripts" ]]; then
    mkdir -p "$USER_HOME/scripts"
    cp -a "$INSTALL_DIR/scripts/." "$USER_HOME/scripts/" 2>/dev/null || true
    echo "==> Copied bundled scripts to $USER_HOME/scripts/"
fi

# Copy example config on first run
if [[ ! -f "$USER_HOME/config.json" ]]; then
    if [[ -f "$INSTALL_DIR/config.json.example" ]]; then
        cp "$INSTALL_DIR/config.json.example" "$USER_HOME/config.json"
        echo "==> Created $USER_HOME/config.json from example"
        echo "    Please edit it to set your API keys."
    else
        echo "WARNING: No config.json.example found in $INSTALL_DIR"
    fi
fi

# Note: the server mints its own self-signed TLS cert on first start when
# certs/j9t-{cert,key}.pem are absent (see WebServer::Start) — no launcher-side
# openssl provisioning is needed.

# ---- Python venv (first-run) ----
if [[ ! -d "$USER_HOME/.venv" ]]; then
    echo "==> Creating Python virtual environment at $USER_HOME/.venv ..."
    python3 -m venv "$USER_HOME/.venv" 2>/dev/null || {
        echo "WARNING: Could not create Python venv (python3-venv may not be installed)"
        echo "         Shell tasks using markitdown will not work until fixed."
    }

    if [[ -d "$USER_HOME/.venv" ]]; then
        "$USER_HOME/.venv/bin/pip" install --quiet --upgrade pip 2>/dev/null || true
        echo "==> Installing Python tools (markitdown) ..."
        "$USER_HOME/.venv/bin/pip" install --quiet "markitdown[all]" 2>/dev/null || true
    fi
fi

# Activate venv if present (adds markitdown etc. to PATH)
if [[ -f "$USER_HOME/.venv/bin/activate" ]]; then
    source "$USER_HOME/.venv/bin/activate"
fi

# ── Install mmdc (mermaid-cli) if missing ───────────────────────────────────
if ! command -v mmdc &>/dev/null; then
    echo "==> Installing mmdc (mermaid-cli) for PDF workflows ..."
    npm install --prefix "$USER_HOME/.npm-tools" @mermaid-js/mermaid-cli@10.x 2>/dev/null \
        || echo "WARNING: mmdc install failed (npm required)"
fi

# Add local npm-tools bin to PATH (mmdc lives here)
if [[ -d "$USER_HOME/.npm-tools/node_modules/.bin" ]]; then
    export PATH="$USER_HOME/.npm-tools/node_modules/.bin:$PATH"
fi

# ── Hint about pandoc if missing ────────────────────────────────────────────
if ! command -v pandoc &>/dev/null; then
    echo ""
    echo "==> WARNING: 'pandoc' is not installed."
    echo "    PDF workflows will fail without it. Please install pandoc and a LaTeX distribution:"
    echo ""
    echo "    Ubuntu/Debian:  sudo apt install pandoc texlive-latex-base texlive-latex-extra texlive-fonts-recommended"
    echo "    Fedora/RHEL:    sudo dnf install pandoc texlive-latex"
    echo "    Arch:           sudo pacman -S pandoc texlive-bin"
    echo ""
fi

# ---- Open dashboard in default browser ----
if [[ "$OPEN_BROWSER" == true ]]; then
    # Delay slightly so the server has time to start
    (sleep 2 && xdg-open "https://localhost:8443" 2>/dev/null) &
fi

# ---- Launch ----
echo "==> Starting JarvisAgent ($EDITION_LABEL edition) in $USER_HOME"
echo "    Dashboard: https://localhost:8443"
if [[ "$EDITION_LABEL" == "Studio" ]]; then
    echo "    Editor:    https://localhost:8443/editor"
fi
echo ""
cd "$USER_HOME"
exec "$USER_HOME/bin/$BINARY_NAME" "${PASSTHROUGH_ARGS[@]}"
