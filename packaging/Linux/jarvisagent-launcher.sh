#!/usr/bin/env bash
# jarvisagent-launcher.sh — User-space launcher for system-wide JarvisAgent installs.
#
# Installed to /usr/bin/jarvisagent by deb/rpm/Arch packages.
# Creates a per-user working directory with symlinks to read-only assets
# in /opt/jarvisagent/ and writable directories for user data.
#
# Usage:
#   jarvisagent                          # default: ~/JarvisAgent
#   jarvisagent --home /path/to/dir      # custom working directory
#   JARVISAGENT_HOME=/tmp/ja jarvisagent # via environment variable
#   jarvisagent --help                   # pass-through to binary
#   jarvisagent --version                # pass-through to binary
#   jarvisagent --no-browser             # skip opening dashboard in browser

set -euo pipefail

INSTALL_DIR="/opt/jarvisagent"
OPEN_BROWSER=true
USER_HOME=""

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
            exec "$INSTALL_DIR/bin/jarvisAgent" "$1"
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

if [[ ! -x "$INSTALL_DIR/bin/jarvisAgent" ]]; then
    echo "Error: $INSTALL_DIR/bin/jarvisAgent not found or not executable."
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
for asset in bin dashboard workflow-editor scripts doc; do
    if [[ -d "$INSTALL_DIR/$asset" ]]; then
        if [[ -L "$USER_HOME/$asset" ]] || [[ ! -e "$USER_HOME/$asset" ]]; then
            ln -sfn "$INSTALL_DIR/$asset" "$USER_HOME/$asset"
        fi
    fi
done

# Ensure bin/jarvisAgent is reachable even if bin/ is a real directory
# (e.g. git clone has bin/Release/ but no bin/jarvisAgent)
if [[ -d "$USER_HOME/bin" && ! -L "$USER_HOME/bin" && ! -e "$USER_HOME/bin/jarvisAgent" ]]; then
    ln -sfn "$INSTALL_DIR/bin/jarvisAgent" "$USER_HOME/bin/jarvisAgent"
fi

# If dashboard/workflow-editor dist/ folders are real directories, offer to
# replace them with symlinks to the installed (packaged) versions.
for ui_dist in dashboard/ui/dist workflow-editor/ui/dist; do
    installed="$INSTALL_DIR/$ui_dist"
    local_dir="$USER_HOME/$ui_dist"
    if [[ -d "$installed" && -d "$local_dir" && ! -L "$local_dir" ]]; then
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

# ---- Python venv (first-run) ----
if [[ ! -d "$USER_HOME/.venv" ]]; then
    echo "==> Creating Python virtual environment at $USER_HOME/.venv ..."
    python3 -m venv "$USER_HOME/.venv" 2>/dev/null || {
        echo "WARNING: Could not create Python venv (python3-venv may not be installed)"
        echo "         Shell tasks using markitdown/md2pdf will not work until fixed."
    }

    if [[ -d "$USER_HOME/.venv" ]]; then
        "$USER_HOME/.venv/bin/pip" install --quiet --upgrade pip 2>/dev/null || true
        echo "==> Installing Python tools (markitdown, md2pdf-mermaid, playwright) ..."
        "$USER_HOME/.venv/bin/pip" install --quiet "markitdown[all]" md2pdf-mermaid playwright 2>/dev/null || true
        "$USER_HOME/.venv/bin/playwright" install chromium 2>/dev/null || true
    fi
fi

# Activate venv if present (adds markitdown, md2pdf etc. to PATH)
if [[ -f "$USER_HOME/.venv/bin/activate" ]]; then
    source "$USER_HOME/.venv/bin/activate"
fi

# ---- Open dashboard in default browser ----
if [[ "$OPEN_BROWSER" == true ]]; then
    # Delay slightly so the server has time to start
    (sleep 2 && xdg-open "http://localhost:8080" 2>/dev/null) &
fi

# ---- Launch ----
echo "==> Starting JarvisAgent in $USER_HOME"
echo "    Dashboard: http://localhost:8080"
echo "    Editor:    http://localhost:8080/editor"
echo ""
cd "$USER_HOME"
exec "$USER_HOME/bin/jarvisAgent" "${PASSTHROUGH_ARGS[@]}"
