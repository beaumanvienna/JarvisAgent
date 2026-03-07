#!/bin/bash
# prerequisites.sh — Install all dependencies needed to build and run
# JarvisAgent on Arch Linux / Manjaro.
#
# Usage:
#   sudo ./prerequisites.sh

set -euo pipefail

echo "==> Installing build + runtime dependencies ..."
pacman -Syu --noconfirm --needed \
    base-devel git wget \
    python python-pip \
    zlib \
    nodejs npm \
    xdg-utils \
    premake

echo ""
echo "==> All prerequisites installed."
echo "    To build:  cd packaging/Linux/Arch && makepkg -si"
echo "    To run:    jarvisagent"
