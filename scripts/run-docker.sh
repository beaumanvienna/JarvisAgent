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
    if ! groups | grep -qw docker; then
        echo "Your user is not in the 'docker' group. Adding now (requires sudo)..."
        sudo usermod -aG docker "$USER"
        echo ""
        echo "==> Group added. Activating and retrying..."
        exec sg docker -c "$0 $*"
    else
        echo "ERROR: Docker daemon is not running. Please start Docker first."
        exit 1
    fi
fi

IMAGE="ghcr.io/beaumanvienna/jarvisagent:latest"
DATA_DIR="${1:-$HOME/JarvisAgent}"
[[ $# -gt 0 ]] && shift

echo "==> Data directory: $DATA_DIR"
mkdir -p "$DATA_DIR"

echo "==> Pulling $IMAGE"
docker pull "$IMAGE"

echo "==> Starting JarvisAgent"
echo "    Dashboard: http://localhost:8080"
echo "    Editor:    http://localhost:8080/editor"
echo ""

exec docker run -it --rm \
  -p 8080:8080 \
  -v "$DATA_DIR:/app" \
  "$IMAGE" "$@"
