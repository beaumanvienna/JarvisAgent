#!/bin/bash
# build-flatpak.sh — Build a Flatpak for JarvisAgent.
#
# Usage:
#   cd packaging/Linux/Flatpak
#   ./build-flatpak.sh
#
# Prerequisites:
#   sudo apt install flatpak flatpak-builder        # Ubuntu/Zorin/Debian
#   sudo dnf install flatpak flatpak-builder        # Fedora
#   sudo pacman -S flatpak flatpak-builder          # Arch/Manjaro
#
#   flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
#   flatpak install flathub org.freedesktop.Sdk//24.08 org.freedesktop.Sdk.Extension.node18//24.08

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MANIFEST="$SCRIPT_DIR/com.jctechnolabs.JarvisAgent.yml"
BUILD_DIR="$SCRIPT_DIR/build"
REPO_DIR="$SCRIPT_DIR/build/repo"
BUNDLE="$SCRIPT_DIR/build/JarvisAgent.flatpak"

# ---- Clean previous build ----
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "==> Building Flatpak from manifest ..."
flatpak-builder \
    --force-clean \
    --repo="$REPO_DIR" \
    "$BUILD_DIR/builddir" \
    "$MANIFEST"

echo "==> Creating single-file Flatpak bundle ..."
flatpak build-bundle \
    "$REPO_DIR" \
    "$BUNDLE" \
    com.jctechnolabs.JarvisAgent

echo ""
echo "==> Flatpak bundle created: $BUNDLE"
echo "    Size: $(du -h "$BUNDLE" | cut -f1)"
echo ""
echo "    Install:"
echo "      flatpak install $BUNDLE"
echo ""
echo "    Run:"
echo "      flatpak run com.jctechnolabs.JarvisAgent"
echo ""
echo "    Uninstall:"
echo "      flatpak uninstall com.jctechnolabs.JarvisAgent"
echo ""
echo "    Data directory: ~/.local/share/jarvisagent/"
