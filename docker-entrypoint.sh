#!/usr/bin/env bash
# docker-entrypoint.sh — First-run seeding and upgrade handling for Docker containers.
#
# Read-only image assets live in /opt/jarvisagent/.
# The working directory /app is the volume mount (~/JarvisAgent on the host).
# This script creates symlinks from /app back to the read-only assets so the
# binary finds dashboard/ui/dist and workflow-editor/ui/dist relative to CWD.
#
# Mirrors the behavior of packaging/Linux/jarvisagent-launcher.sh:
# copies example workflows and default config on first run.
#
# On image upgrade (version change), example workflows are re-seeded.

set -euo pipefail

IMAGE_DIR="/opt/jarvisagent"
IMAGE_VERSION_FILE="$IMAGE_DIR/.image-defaults/.image-version"
DATA_VERSION_FILE="/app/.image-version"

# ---- Symlink read-only assets into the working directory ----
# These are re-created on every start (cheap and idempotent).
ln -sfn "$IMAGE_DIR/dashboard"       /app/dashboard
ln -sfn "$IMAGE_DIR/workflow-editor"  /app/workflow-editor
ln -sfn "$IMAGE_DIR/scripts"          /app/scripts

# ---- Determine if seeding is needed ----
NEED_SEED=false

if [ -z "$(ls -A /app/workflows 2>/dev/null)" ]; then
    # First run: workflows directory is empty
    NEED_SEED=true
    echo "==> First run detected — seeding example workflows"
elif [ -f "$IMAGE_VERSION_FILE" ]; then
    IMAGE_VER=$(cat "$IMAGE_VERSION_FILE" 2>/dev/null || echo "")
    DATA_VER=$(cat "$DATA_VERSION_FILE" 2>/dev/null || echo "")
    if [ "$IMAGE_VER" != "$DATA_VER" ]; then
        NEED_SEED=true
        echo "==> Image upgrade detected ($DATA_VER -> $IMAGE_VER) — re-seeding example workflows"
    fi
fi

if [ "$NEED_SEED" = true ]; then
    mkdir -p /app/workflows
    cp -a "$IMAGE_DIR/.image-defaults/workflows/"* /app/workflows/ 2>/dev/null || true
    echo "==> Seeded example workflows into /app/workflows/"

    # Copy version marker so we don't re-seed again until the next upgrade
    if [ -f "$IMAGE_VERSION_FILE" ]; then
        cp "$IMAGE_VERSION_FILE" "$DATA_VERSION_FILE"
    fi
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
    chown -R "$MOUNT_UID:$MOUNT_GID" /app/workflows /app/queue /app/log 2>/dev/null || true
    chown "$MOUNT_UID:$MOUNT_GID" /app/config.json /app/.image-version 2>/dev/null || true
fi

# ---- Launch ----
echo "==> Starting JarvisAgent"
echo "    Dashboard: http://localhost:8080"
echo "    Editor:    http://localhost:8080/editor"
echo ""

# Drop privileges to match the host user's UID/GID so all runtime-created
# files (keys.json.enc, workflow outputs, logs) are owned by the host user.
if [ "$MOUNT_UID" != "0" ]; then
    exec gosu "$MOUNT_UID:$MOUNT_GID" "$IMAGE_DIR/jarvisAgent-studio" "$@"
else
    exec "$IMAGE_DIR/jarvisAgent-studio" "$@"
fi
