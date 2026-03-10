#!/usr/bin/env bash
# docker-entrypoint.sh — First-run seeding for Docker containers.
#
# Read-only image assets live in /opt/jarvisagent/.
# The working directory /app is the volume mount (~/JarvisAgent on the host).
# This script creates symlinks from /app back to the read-only assets so the
# binary finds dashboard/ui/dist and workflow-editor/ui/dist relative to CWD.
#
# Mirrors the behavior of packaging/Linux/jarvisagent-launcher.sh:
# copies example workflows and default config on first run.

set -euo pipefail

IMAGE_DIR="/opt/jarvisagent"

# ---- Symlink read-only assets into the working directory ----
# These are re-created on every start (cheap and idempotent).
ln -sfn "$IMAGE_DIR/dashboard"       /app/dashboard
ln -sfn "$IMAGE_DIR/workflow-editor"  /app/workflow-editor
ln -sfn "$IMAGE_DIR/scripts"          /app/scripts

# ---- First-run seeding ----
# Copy example workflows on first run (only if workflows dir is empty)
if [ -z "$(ls -A /app/workflows 2>/dev/null)" ]; then
    mkdir -p /app/workflows
    cp -a "$IMAGE_DIR/.image-defaults/workflows/"* /app/workflows/ 2>/dev/null || true
    echo "==> Seeded example workflows into /app/workflows/"
fi

# Copy example config on first run
if [ ! -f /app/config.json ] && [ -f "$IMAGE_DIR/.image-defaults/config.json" ]; then
    cp "$IMAGE_DIR/.image-defaults/config.json" /app/config.json
    echo "==> Created /app/config.json from defaults"
fi

# Ensure writable directories exist
mkdir -p /app/queue /app/log

# ---- Fix ownership to match host user ----
# If the user pre-created ~/JarvisAgent (recommended), /app is owned by their
# UID. Chown all seeded files to match so the host user can manage them.
MOUNT_UID=$(stat -c '%u' /app)
MOUNT_GID=$(stat -c '%g' /app)
if [ "$MOUNT_UID" != "0" ]; then
    chown -R "$MOUNT_UID:$MOUNT_GID" /app/workflows /app/config.json /app/queue /app/log 2>/dev/null || true
fi

# ---- Launch ----
echo "==> Starting JarvisAgent"
echo "    Dashboard: http://localhost:8080"
echo "    Editor:    http://localhost:8080/editor"
echo ""
exec "$IMAGE_DIR/jarvisAgent" "$@"
