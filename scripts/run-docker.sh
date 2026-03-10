#!/usr/bin/env bash
# run-docker.sh — Pull and run JarvisAgent via Docker.
#
# Usage:
#   ./scripts/run-docker.sh          # uses ~/JarvisAgent as data directory
#   ./scripts/run-docker.sh /path    # uses custom data directory

set -euo pipefail

IMAGE="ghcr.io/beaumanvienna/jarvisagent:latest"
DATA_DIR="${1:-$HOME/JarvisAgent}"

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
