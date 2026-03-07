#!/bin/bash
# prerequisites.sh — Install all dependencies needed to build and run
# JarvisAgent on Rocky Linux / RHEL / Fedora.
#
# Usage:
#   sudo ./prerequisites.sh

set -euo pipefail

echo "==> Installing build + runtime dependencies ..."
dnf install -y \
    gcc gcc-c++ make git wget tar \
    python3 python3-devel python3-pip \
    zlib zlib-devel \
    rpm-build \
    bash xdg-utils

echo "==> Installing Node.js ..."
if dnf module list nodejs &>/dev/null 2>&1; then
    # RHEL/Rocky 9 — use module stream
    dnf module reset -y nodejs
    dnf module enable -y nodejs:20
    dnf install -y nodejs npm
else
    # RHEL/Rocky 10+ — modularity removed, install from AppStream or NodeSource
    if dnf install -y nodejs npm 2>/dev/null; then
        echo "    Node.js installed from default repos"
    else
        echo "    Node.js not in repos — installing from NodeSource ..."
        curl -fsSL https://rpm.nodesource.com/setup_20.x | bash -
        dnf install -y nodejs
    fi
fi

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
echo "    To build:  cd packaging/Linux/RPM && ./build-rpm.sh"
echo "    To run:    jarvisagent"
