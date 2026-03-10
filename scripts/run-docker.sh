#!/usr/bin/env bash
# run-docker.sh — Pull and run JarvisAgent via Docker.
#
# Usage:
#   ./scripts/run-docker.sh          # uses ~/JarvisAgent as data directory
#   ./scripts/run-docker.sh /path    # uses custom data directory

set -euo pipefail

# ---- Pre-flight: check Docker is available ----
if ! command -v docker &>/dev/null; then
    echo "ERROR: 'docker' not found. Please install Docker first:"
    echo "  https://docs.docker.com/engine/install/"
    exit 1
fi

if ! docker info &>/dev/null; then
    echo "Cannot connect to the Docker daemon."
    echo ""
    if ! id -nG "$USER" | grep -qw docker; then
        echo "Your user is not in the 'docker' group. Adding now (requires sudo)..."
        sudo usermod -aG docker "$USER"
    fi
    # Group may exist but not be active in this shell — re-exec under it.
    # The guard variable prevents an infinite loop if the daemon is genuinely stopped.
    if id -nG "$USER" | grep -qw docker && [ -z "${_RUN_DOCKER_REEXEC:-}" ]; then
        echo "==> Activating docker group..."
        export _RUN_DOCKER_REEXEC=1
        exec sg docker -c "$0 $*"
    fi
    echo "ERROR: Docker daemon is not running. Please start Docker first."
    exit 1
fi

IMAGE="ghcr.io/beaumanvienna/jarvisagent:latest"
DATA_DIR="${1:-$HOME/JarvisAgent}"
[[ $# -gt 0 ]] && shift

echo "==> Data directory: $DATA_DIR"
mkdir -p "$DATA_DIR"

echo "==> Pulling $IMAGE"
if ! docker pull "$IMAGE" 2>/dev/null; then
    if docker image inspect "$IMAGE" &>/dev/null; then
        echo "WARNING: Pull failed (no internet?). Using cached image."
    else
        echo "ERROR: Pull failed and no cached image found. Check your internet connection."
        exit 1
    fi
fi

echo "==> Starting JarvisAgent"
echo "    Dashboard: http://localhost:8080"
echo "    Editor:    http://localhost:8080/editor"
echo ""

# Check if port 8080 is already in use
if command -v ss &>/dev/null && ss -tlnp 2>/dev/null | grep -q ':8080 '; then
    echo "ERROR: Port 8080 is already in use. Stop the other process first."
    exit 1
fi

exec docker run -it --rm \
  -p 8080:8080 \
  -v "$DATA_DIR:/app" \
  "$IMAGE" "$@"
