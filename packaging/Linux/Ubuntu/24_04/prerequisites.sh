#!/bin/bash
# prerequisites.sh — Install all dependencies needed to build and run
# JarvisAgent on Ubuntu / Zorin / Debian.
#
# Usage:
#   sudo ./prerequisites.sh

set -euo pipefail

echo "==> Installing build + runtime dependencies ..."
apt-get update
apt-get install -y \
    build-essential git wget \
    python3 python3-dev python3-pip python3-venv \
    zlib1g-dev \
    nodejs npm \
    xdg-utils

echo "==> Installing Premake5 ..."
if ! command -v premake5 &>/dev/null; then
    cd /tmp
    wget -q https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-linux.tar.gz
    tar -xzf premake-5.0.0-beta2-linux.tar.gz
    mv premake5 /usr/local/bin/
    chmod +x /usr/local/bin/premake5
    echo "    premake5 installed to /usr/local/bin/"
else
    echo "    premake5 already installed: $(which premake5)"
fi

echo ""
echo "==> All prerequisites installed."
echo "    To build:  cd packaging/Linux/Ubuntu/24_04 && ./build-deb.sh"
echo "    To run:    jarvisagent"
