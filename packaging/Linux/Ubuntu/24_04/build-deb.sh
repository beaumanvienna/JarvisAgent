#!/bin/bash
# build-deb.sh — Build a .deb package for JarvisAgent from the current source tree.
#
# Usage:
#   cd packaging/Linux/Ubuntu/24_04
#   ./build-deb.sh [--dry-run]
#
# Prerequisites (Ubuntu/Zorin/Debian):
#   sudo apt install -y build-essential python3 python3-dev python3-pip python3-venv \
#       zlib1g-dev nodejs npm
#   premake5 must be on PATH (build from source or download binary)
#
# Options:
#   --dry-run   Assemble the package tree and validate with dpkg-deb but skip
#               the actual C++ and React builds (uses existing build artifacts).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
PKG_NAME="jarvisagent"
PKG_VERSION=$(grep 'JARVIS_AGENT_VERSION' "$REPO_ROOT/premake5.lua" | sed 's/.*\\"\(.*\)\\".*$/\1/')
if [[ -z "$PKG_VERSION" ]]; then echo "ERROR: could not extract version from premake5.lua"; exit 1; fi
PKG_RELEASE="1"
PKG_ARCH="amd64"
DEB_NAME="${PKG_NAME}_${PKG_VERSION}-${PKG_RELEASE}_${PKG_ARCH}"
BUILD_DIR="$SCRIPT_DIR/build/${DEB_NAME}"

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=true
    echo "==> DRY RUN: skipping C++ and React builds, using existing artifacts"
fi

# ---- Clean previous build ----
rm -rf "$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"

# ---- Build from source (unless dry-run) ----
if [[ "$DRY_RUN" == false ]]; then
    cd "$REPO_ROOT"

    echo "==> Building Engine edition ..."
    premake5 gmake --engine
    make -j"$(nproc)" config=release

    echo "==> Building Studio edition ..."
    premake5 gmake
    make -j"$(nproc)" config=release

    echo "==> Building React dashboard ..."
    cd "$REPO_ROOT/dashboard/ui"
    npm install
    npm run build

    echo "==> Building React workflow editor ..."
    cd "$REPO_ROOT/workflow-editor/ui"
    npm install
    npm run build
fi

# ---- Assemble package tree ----
echo "==> Assembling package tree ..."

INST="$BUILD_DIR/opt/jarvisagent"
mkdir -p "$INST/bin"
mkdir -p "$INST/dashboard/ui"
mkdir -p "$INST/workflow-editor/ui"
mkdir -p "$INST/doc"
mkdir -p "$INST/queue"
mkdir -p "$INST/log"
mkdir -p "$BUILD_DIR/usr/bin"
mkdir -p "$BUILD_DIR/DEBIAN"

# Binaries (Studio + Engine)
for bin in jarvisAgent-studio jarvisAgent-engine; do
    if [[ -f "$REPO_ROOT/bin/Release/$bin" ]]; then
        cp "$REPO_ROOT/bin/Release/$bin" "$INST/bin/$bin"
        chmod 755 "$INST/bin/$bin"
    else
        echo "WARNING: bin/Release/$bin not found (expected in dry-run)"
    fi
done

# React UIs
if [[ -d "$REPO_ROOT/dashboard/ui/dist" ]]; then
    cp -r "$REPO_ROOT/dashboard/ui/dist" "$INST/dashboard/ui/dist"
else
    echo "WARNING: dashboard/ui/dist not found (expected in dry-run)"
fi

if [[ -d "$REPO_ROOT/workflow-editor/ui/dist" ]]; then
    cp -r "$REPO_ROOT/workflow-editor/ui/dist" "$INST/workflow-editor/ui/dist"
else
    echo "WARNING: workflow-editor/ui/dist not found (expected in dry-run)"
fi

# Scripts (excluding __pycache__)
cp -r "$REPO_ROOT/scripts" "$INST/scripts"
find "$INST/scripts" -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
chmod +x "$INST/scripts/"*.sh

# Example workflows (curated list — no subdirs, no build artifacts)
mkdir -p "$INST/workflows"
for jcwf in aiCarMaintenancePipeline aiZipDemo \
            make-example portfolioDividendAnalysis; do
    cp "$REPO_ROOT/example/workflows/${jcwf}.jcwf" "$INST/workflows/" 2>/dev/null || true
done
# Loose input files are now bundled inside the .jcwf zip containers.

# Example config
cp "$REPO_ROOT/packaging/config.json.example" "$INST/config.json.example"

# Documentation
cp "$REPO_ROOT/README.md" "$INST/doc/README.md"
cp "$REPO_ROOT/doc/JC_Workflow_Specification.md" "$INST/doc/JC_Workflow_Specification.md" 2>/dev/null || true

# Man page
mkdir -p "$BUILD_DIR/usr/share/man/man1"
cp "$REPO_ROOT/doc/jarvisagent.1" "$BUILD_DIR/usr/share/man/man1/jarvisagent.1"
gzip -9 "$BUILD_DIR/usr/share/man/man1/jarvisagent.1"

# DEBIAN control files
cp "$SCRIPT_DIR/DEBIAN/control" "$BUILD_DIR/DEBIAN/control"
sed -i "s/^Version:.*/Version: ${PKG_VERSION}-${PKG_RELEASE}/" "$BUILD_DIR/DEBIAN/control"
cp "$SCRIPT_DIR/DEBIAN/postinst" "$BUILD_DIR/DEBIAN/postinst"
cp "$SCRIPT_DIR/DEBIAN/postrm" "$BUILD_DIR/DEBIAN/postrm"
chmod 755 "$BUILD_DIR/DEBIAN/postinst"
chmod 755 "$BUILD_DIR/DEBIAN/postrm"

# Launcher scripts (shared across deb/rpm/arch — detects edition from script name)
install -m755 "$SCRIPT_DIR/../../jarvisagent-launcher.sh" "$BUILD_DIR/usr/bin/jarvisagent"
install -m755 "$SCRIPT_DIR/../../jarvisagent-launcher.sh" "$BUILD_DIR/usr/bin/jarvisagent-engine"
ln -sf jarvisagent "$BUILD_DIR/usr/bin/jarvisagent-studio"
ln -sf jarvisagent "$BUILD_DIR/usr/bin/j9t"

# ---- Build .deb ----
echo "==> Building .deb package ..."
cd "$SCRIPT_DIR/build"
dpkg-deb --build "$DEB_NAME"

DEB_PATH="$SCRIPT_DIR/build/${DEB_NAME}.deb"
echo ""
echo "==> Package created: $DEB_PATH"
echo "    Size: $(du -h "$DEB_PATH" | cut -f1)"
echo ""
echo "    Install:   sudo dpkg -i $DEB_PATH"
echo "    Fix deps:  sudo apt install -f"
echo "    Remove:    sudo apt remove jarvisagent"
echo ""

# ---- Validate ----
echo "==> Package info:"
dpkg-deb --info "$DEB_PATH"
echo ""
echo "==> Package contents (first 40 entries):"
dpkg-deb --contents "$DEB_PATH" 2>/dev/null | head -40 || true
