#!/bin/bash
# jarvisagent-wrapper.sh — Flatpak entry point for JarvisAgent.
#
# Sets up a working directory at ~/JarvisAgent with symlinks
# to read-only assets inside the Flatpak and real directories for writable
# data (queue/, log/, workflows/).

set -euo pipefail

SHARE="/app/share/jarvisagent"
DATA_DIR="${JARVISAGENT_DATA:-$HOME/JarvisAgent}"

# ---- Argument handling (before any first-run setup) ----
# The wrapper owns arg policy so the slow first-run scaffold (data dir + symlinks +
# Python venv) never runs just to print --help/--version or to reject a typo:
#   --help/-h, --version/-v  j9t info flags. exec the real binary straight away —
#                            these return before j9t's config.json check, so no
#                            working directory or venv is needed.
#   --home <dir>             wrapper flag: working directory to install JarvisAgent
#                            into (must be under $HOME — the sandbox grants
#                            --filesystem=home). Overrides the JARVISAGENT_DATA env.
#   --no-browser             wrapper flag, accepted as a no-op (this launcher opens
#                            no browser); kept for invocation parity with the other
#                            launchers.
#   anything else            unknown: strict exit 1 (Unix-style, mirrors j9t itself).
# j9t takes no positional arguments, so a non-flag argument is rejected too.
while [[ $# -gt 0 ]]; do
    case "$1" in
        --help|-h|--version|-v)
            exec "$SHARE/bin/jarvisAgent-studio" "$1"
            ;;
        --home)
            if [[ -z "${2:-}" ]]; then
                echo "jarvisagent: --home requires a directory argument" >&2
                exit 1
            fi
            DATA_DIR="$2"
            shift 2
            ;;
        --no-browser)
            shift  # no-op: the Flatpak never opens a browser
            ;;
        *)
            echo "jarvisagent: unknown option '$1'" >&2
            echo "Try 'flatpak run com.jctechnolabs.JarvisAgent --help' for more information." >&2
            exit 1
            ;;
    esac
done

# ---- First-run setup ----
if [[ ! -d "$DATA_DIR" ]]; then
    echo "==> First run: creating working directory at $DATA_DIR"
    mkdir -p "$DATA_DIR"
fi

# Symlink read-only assets from Flatpak into the working directory.
# Uses ln -sfn to atomically replace existing symlinks without rm.
# If a real directory already exists, skip it — content is there.
# NOTE: scripts/ is deliberately NOT symlinked — see the copy step below.
for asset in bin dashboard workflow-editor doc; do
    if [[ -L "$DATA_DIR/$asset" ]] || [[ ! -e "$DATA_DIR/$asset" ]]; then
        ln -sfn "$SHARE/$asset" "$DATA_DIR/$asset"
    fi
done

# Create writable directories if they don't exist
mkdir -p "$DATA_DIR/queue"
mkdir -p "$DATA_DIR/log"
mkdir -p "$DATA_DIR/workflows"

# Copy example workflows on first run
if [[ -d "$SHARE/example-workflows" ]] && [[ -z "$(ls -A "$DATA_DIR/workflows" 2>/dev/null)" ]]; then
    cp -a "$SHARE/example-workflows/"* "$DATA_DIR/workflows/" 2>/dev/null || true
    echo "==> Copied example workflows to $DATA_DIR/workflows/"
fi

# Copy bundled scripts as REAL files (not a symlink). Workflow shell/python tasks
# reference scripts/ via path-confined relative paths; confinement follows a
# symlink out of the working dir and rejects it, breaking every script task.
if [[ -L "$DATA_DIR/scripts" ]]; then rm -f "$DATA_DIR/scripts"; fi  # drop a stale symlink from an older install
if [[ -d "$SHARE/scripts" ]] && [[ ! -d "$DATA_DIR/scripts" ]]; then
    mkdir -p "$DATA_DIR/scripts"
    cp -a "$SHARE/scripts/." "$DATA_DIR/scripts/" 2>/dev/null || true
    echo "==> Copied bundled scripts to $DATA_DIR/scripts/"
fi

# Copy example config on first run
if [[ ! -f "$DATA_DIR/config.json" ]]; then
    cp "$SHARE/config.json.example" "$DATA_DIR/config.json"
    echo "==> Created $DATA_DIR/config.json from example — please edit it"
    echo "    (set API keys, adjust queue/workflow paths if needed)"
fi

# Note: the server mints its own self-signed TLS cert on first start when
# certs/j9t-{cert,key}.pem are absent (see WebServer::Start). This is why no
# openssl CLI is needed inside the Flatpak sandbox.

# ---- Python venv (first-run) ----
if [[ ! -d "$DATA_DIR/.venv" ]]; then
    echo "==> Creating Python virtual environment at $DATA_DIR/.venv ..."
    python3 -m venv "$DATA_DIR/.venv" 2>/dev/null || {
        echo "WARNING: Could not create Python venv"
        echo "         Shell tasks using markitdown will not work until fixed."
    }

    if [[ -d "$DATA_DIR/.venv" ]]; then
        "$DATA_DIR/.venv/bin/pip" install --quiet --upgrade pip 2>/dev/null || true
        echo "==> Installing Python tools (markitdown) ..."
        "$DATA_DIR/.venv/bin/pip" install --quiet "markitdown[all]" 2>/dev/null || true
    fi
fi

# Activate venv if present
if [[ -f "$DATA_DIR/.venv/bin/activate" ]]; then
    source "$DATA_DIR/.venv/bin/activate"
fi

# ---- Launch ----
# All arguments were handled above (info flags exec'd early, wrapper flags consumed,
# unknown args rejected), so j9t starts with no arguments of its own.
cd "$DATA_DIR"
exec "$DATA_DIR/bin/jarvisAgent-studio"
