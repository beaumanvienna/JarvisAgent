#!/usr/bin/env bash
# docker-entrypoint.sh — First-run seeding and upgrade handling for Docker containers.
#
# Read-only image assets live in /opt/jarvisagent/.
# The working directory /app is the volume mount (~/JarvisAgent on the host).
# This script creates symlinks from /app back to the read-only assets so the
# binary finds the UI dist/ folders relative to CWD (flat install layout).
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

# Copy example config on first run. The shared config.json.example ships with
# TLS enabled (native installs serve HTTPS on 8443 and the server mints its own
# self-signed cert on first start). Docker defaults to plain HTTP on 8080, so we
# STRIP the TLS fields unless the container was started with --tls (J9T_TLS=1).
if [ ! -f /app/config.json ] && [ -f "$IMAGE_DIR/.image-defaults/config.json" ]; then
    cp "$IMAGE_DIR/.image-defaults/config.json" /app/config.json
    if [ "${J9T_TLS:-0}" = "1" ]; then
        python3 - <<'PY'
import json
path = "/app/config.json"
with open(path) as f:
    cfg = json.load(f)
cfg["port"] = 8443
cfg["TlsCert"] = "certs/j9t-cert.pem"
cfg["TlsKey"] = "certs/j9t-key.pem"
with open(path, "w") as f:
    json.dump(cfg, f, indent=4)
PY
        echo "==> Seeded /app/config.json with TLS defaults (port 8443)"
    else
        python3 - <<'PY'
import json
path = "/app/config.json"
with open(path) as f:
    cfg = json.load(f)
for k in ("port", "TlsCert", "TlsKey"):
    cfg.pop(k, None)
with open(path, "w") as f:
    json.dump(cfg, f, indent=4)
PY
        echo "==> Created /app/config.json from defaults (HTTP mode — TLS fields removed)"
    fi
fi

# Detect mode/config mismatch — we fail fast with instructions rather than
# let the binary crash with a cryptic TLS-cert-not-found error.
HAS_TLS=no
if grep -q '"TlsCert"' /app/config.json 2>/dev/null; then
    HAS_TLS=yes
fi

if [ "${J9T_TLS:-0}" = "1" ] && [ "$HAS_TLS" = "no" ]; then
    cat <<'EOF' >&2

==============================================================================
ERROR: TLS mode requested, but /app/config.json has no TLS configuration.

You ran the container with --tls (J9T_TLS=1), but the existing config file
disables TLS. The config was seeded on an earlier run without --tls and is
not re-seeded automatically (your data directory is persistent).

Your config file on the host is at:
    ~/JarvisAgent/config.json   (or whichever data dir you passed)

Fix ONE of the following:

  1. Enable TLS in the config — add these fields (before the closing brace):
         "port": 8443,
         "TlsCert": "certs/j9t-cert.pem",
         "TlsKey": "certs/j9t-key.pem"

  2. Delete ~/JarvisAgent/config.json and re-run with --tls; the entrypoint
     will re-seed it with TLS defaults.

See the DOCKER section of the user manual for details:
    doc/jarvisagent.md   (or `man jarvisagent` on Linux/macOS)
==============================================================================
EOF
    exit 1
fi

if [ "${J9T_TLS:-0}" != "1" ] && [ "$HAS_TLS" = "yes" ]; then
    cat <<'EOF' >&2

==============================================================================
ERROR: /app/config.json enables TLS, but the container was not started with
--tls. The server would try to bind HTTPS on 8443, but the host mapped 8080
and the certs directory is not expected to be present.

Your config file on the host is at:
    ~/JarvisAgent/config.json   (or whichever data dir you passed)

Fix ONE of the following:

  1. Re-run the container with TLS enabled:
         ./scripts/run-docker.sh --tls

  2. Disable TLS in the config — remove these lines from the file:
         "port": 8443,
         "TlsCert": ...,
         "TlsKey": ...

See the DOCKER section of the user manual for details:
    doc/jarvisagent.md   (or `man jarvisagent` on Linux/macOS)
==============================================================================
EOF
    exit 1
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
if [ "${J9T_TLS:-0}" = "1" ]; then
    echo "    Dashboard: https://localhost:8443"
    echo "    Editor:    https://localhost:8443/editor"
else
    echo "    Dashboard: http://localhost:8080"
    echo "    Editor:    http://localhost:8080/editor"
fi
echo ""

# Drop privileges to match the host user's UID/GID so all runtime-created
# files (keys.json.enc, workflow outputs, logs) are owned by the host user.
if [ "$MOUNT_UID" != "0" ]; then
    exec gosu "$MOUNT_UID:$MOUNT_GID" "$IMAGE_DIR/jarvisAgent-studio" "$@"
else
    exec "$IMAGE_DIR/jarvisAgent-studio" "$@"
fi
