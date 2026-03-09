#!/usr/bin/env bash
# docker-entrypoint.sh — First-run seeding for Docker containers.
#
# Mirrors the behavior of packaging/Linux/jarvisagent-launcher.sh:
# copies example workflows and default config into the mounted volume
# on first run, then launches jarvisAgent.

set -euo pipefail

# ---- First-run seeding ----
# When ~/JarvisAgent is mounted at /app, these directories may be empty.
# Seed them from the read-only image assets stored in /app/.image-defaults/.

DEFAULTS_DIR="/app/.image-defaults"

# Copy example workflows on first run (only if workflows dir is empty)
if [ -d "$DEFAULTS_DIR/workflows" ] && [ -z "$(ls -A /app/workflows 2>/dev/null)" ]; then
    mkdir -p /app/workflows
    cp -a "$DEFAULTS_DIR/workflows/"* /app/workflows/ 2>/dev/null || true
    echo "==> Copied example workflows to /app/workflows/"
fi

# Copy example config on first run
if [ ! -f /app/config.json ] && [ -f "$DEFAULTS_DIR/config.json" ]; then
    cp "$DEFAULTS_DIR/config.json" /app/config.json
    echo "==> Created /app/config.json from example"
fi

# Symlink scripts to image's read-only copy if not present
if [ ! -e /app/scripts ] && [ -d "$DEFAULTS_DIR/scripts" ]; then
    ln -sfn "$DEFAULTS_DIR/scripts" /app/scripts
    echo "==> Linked /app/scripts to image defaults"
fi

# Ensure writable directories exist
mkdir -p /app/queue /app/log

# ---- Launch ----
echo "==> Starting JarvisAgent"
echo "    Dashboard: http://localhost:8080"
echo "    Editor:    http://localhost:8080/editor"
echo ""
exec ./jarvisAgent "$@"
