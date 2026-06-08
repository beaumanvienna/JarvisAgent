#!/bin/bash
# build-rpm.sh — Build an .rpm package for JarvisAgent from the current source tree.
#
# Usage:
#   cd packaging/Linux/RPM
#   ./build-rpm.sh [--dry-run]
#
# Prerequisites (Fedora/RHEL/Rocky):
#   sudo dnf install -y gcc gcc-c++ make python3 python3-devel python3-pip \
#       zlib-devel nodejs npm rpm-build
#   premake5 must be on PATH (build from source or download binary)
#
# Options:
#   --dry-run   Assemble the package tree and create the RPM using existing
#               build artifacts (skips C++ and React builds).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PKG_NAME="jarvisagent"
PKG_VERSION=$(grep 'JARVIS_AGENT_VERSION' "$REPO_ROOT/premake5.lua" | sed 's/.*\\"\(.*\)\\".*$/\1/')
if [[ -z "$PKG_VERSION" ]]; then echo "ERROR: could not extract version from premake5.lua"; exit 1; fi
PKG_RELEASE="1"
PKG_ARCH="x86_64"
RPM_NAME="${PKG_NAME}-${PKG_VERSION}-${PKG_RELEASE}.${PKG_ARCH}"
BUILD_DIR="$SCRIPT_DIR/build"
STAGING="$BUILD_DIR/staging"

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=true
    echo "==> DRY RUN: skipping C++ and React builds, using existing artifacts"
fi

# ---- Clean previous build ----
rm -rf "$BUILD_DIR"
mkdir -p "$STAGING/opt/jarvisagent/bin"
mkdir -p "$STAGING/opt/jarvisagent/dashboard/ui"
mkdir -p "$STAGING/opt/jarvisagent/workflow-editor/ui"
mkdir -p "$STAGING/opt/jarvisagent/doc"
mkdir -p "$STAGING/opt/jarvisagent/queue"
mkdir -p "$STAGING/opt/jarvisagent/log"
mkdir -p "$STAGING/opt/jarvisagent/workflows"
mkdir -p "$STAGING/usr/bin"

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
    cd "$REPO_ROOT/code/frontend/dashboard/ui"
    npm install
    npm run build

    echo "==> Building React workflow editor ..."
    cd "$REPO_ROOT/code/frontend/workflow-editor/ui"
    npm install
    npm run build
fi

# ---- Assemble package tree ----
echo "==> Assembling package tree ..."

# Binaries (Studio + Engine)
for bin in jarvisAgent-studio jarvisAgent-engine; do
    if [[ -f "$REPO_ROOT/bin/Release/$bin" ]]; then
        cp "$REPO_ROOT/bin/Release/$bin" "$STAGING/opt/jarvisagent/bin/$bin"
        chmod 755 "$STAGING/opt/jarvisagent/bin/$bin"
    else
        echo "WARNING: bin/Release/$bin not found (expected in dry-run)"
    fi
done

# React UIs
if [[ -d "$REPO_ROOT/code/frontend/dashboard/ui/dist" ]]; then
    cp -r "$REPO_ROOT/code/frontend/dashboard/ui/dist" "$STAGING/opt/jarvisagent/dashboard/ui/dist"
else
    echo "WARNING: code/frontend/dashboard/ui/dist not found (expected in dry-run)"
fi

if [[ -d "$REPO_ROOT/code/frontend/workflow-editor/ui/dist" ]]; then
    cp -r "$REPO_ROOT/code/frontend/workflow-editor/ui/dist" "$STAGING/opt/jarvisagent/workflow-editor/ui/dist"
else
    echo "WARNING: code/frontend/workflow-editor/ui/dist not found (expected in dry-run)"
fi

# Scripts (excluding __pycache__)
cp -r "$REPO_ROOT/scripts" "$STAGING/opt/jarvisagent/scripts"
find "$STAGING/opt/jarvisagent/scripts" -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
chmod +x "$STAGING/opt/jarvisagent/scripts/"*.sh

# Example workflows (curated set — single source: packaging/curated-workflows.txt)
while IFS= read -r jcwf; do
    case "$jcwf" in ''|\#*) continue;; esac
    cp "$REPO_ROOT/example/workflows/${jcwf}.jcwf" "$STAGING/opt/jarvisagent/workflows/" 2>/dev/null || true
done < "$REPO_ROOT/packaging/curated-workflows.txt"
# (Input files are bundled inside each .jcwf zip — no loose files to copy)

# Example config
cp "$REPO_ROOT/packaging/config.json.example" "$STAGING/opt/jarvisagent/config.json.example"

# Documentation
cp "$REPO_ROOT/README.md" "$STAGING/opt/jarvisagent/doc/README.md"
cp "$REPO_ROOT/doc/JC_Workflow_Specification.md" "$STAGING/opt/jarvisagent/doc/JC_Workflow_Specification.md" 2>/dev/null || true

# Man page
mkdir -p "$STAGING/usr/share/man/man1"
cp "$REPO_ROOT/doc/jarvisagent.1" "$STAGING/usr/share/man/man1/jarvisagent.1"
gzip -9 "$STAGING/usr/share/man/man1/jarvisagent.1"

# Launcher scripts (shared across deb/rpm/arch — detects edition from script name)
install -m755 "$SCRIPT_DIR/../jarvisagent-launcher.sh" "$STAGING/usr/bin/jarvisagent"
install -m755 "$SCRIPT_DIR/../jarvisagent-launcher.sh" "$STAGING/usr/bin/jarvisagent-engine"
ln -sf jarvisagent "$STAGING/usr/bin/jarvisagent-studio"
ln -sf jarvisagent "$STAGING/usr/bin/j9t"

# ---- Build RPM using rpmbuild (if available) or fpm ----
if command -v rpmbuild &>/dev/null; then
    echo "==> Building RPM with rpmbuild ..."

    RPMBUILD_DIR="$BUILD_DIR/rpmbuild"
    mkdir -p "$RPMBUILD_DIR"/{SPECS,RPMS,BUILD,BUILDROOT,SOURCES}

    cp "$SCRIPT_DIR/jarvisagent.spec" "$RPMBUILD_DIR/SPECS/"
    sed -i "s/^Version:.*/Version:        $PKG_VERSION/" "$RPMBUILD_DIR/SPECS/jarvisagent.spec"

    # Populate BUILD/JarvisAgent with repo-layout files that %install expects.
    # rpmbuild 4.19+ always wipes BUILDROOT before %install, so we cannot
    # pre-populate it.  Instead we place files where the spec's %install
    # section can find them (same layout as the repo after a build).
    SRCDIR="$RPMBUILD_DIR/BUILD/JarvisAgent"
    mkdir -p "$SRCDIR"

    # Binaries (Studio + Engine)
    mkdir -p "$SRCDIR/bin/Release"
    for bin in jarvisAgent-studio jarvisAgent-engine; do
        if [[ -f "$REPO_ROOT/bin/Release/$bin" ]]; then
            cp "$REPO_ROOT/bin/Release/$bin" "$SRCDIR/bin/Release/$bin"
        fi
    done

    # React UIs
    if [[ -d "$REPO_ROOT/code/frontend/dashboard/ui/dist" ]]; then
        mkdir -p "$SRCDIR/code/frontend/dashboard/ui"
        cp -r "$REPO_ROOT/code/frontend/dashboard/ui/dist" "$SRCDIR/code/frontend/dashboard/ui/dist"
    fi
    if [[ -d "$REPO_ROOT/code/frontend/workflow-editor/ui/dist" ]]; then
        mkdir -p "$SRCDIR/code/frontend/workflow-editor/ui"
        cp -r "$REPO_ROOT/code/frontend/workflow-editor/ui/dist" "$SRCDIR/code/frontend/workflow-editor/ui/dist"
    fi

    # Scripts
    cp -r "$REPO_ROOT/scripts" "$SRCDIR/scripts"
    find "$SRCDIR/scripts" -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true

    # Example workflows (curated set — single source: packaging/curated-workflows.txt)
    mkdir -p "$SRCDIR/example/workflows"
    while IFS= read -r jcwf; do
        case "$jcwf" in ''|\#*) continue;; esac
        cp "$REPO_ROOT/example/workflows/${jcwf}.jcwf" "$SRCDIR/example/workflows/" 2>/dev/null || true
    done < "$REPO_ROOT/packaging/curated-workflows.txt"

    # Config + docs
    cp "$REPO_ROOT/packaging/config.json.example" "$SRCDIR/config.json"
    cp "$REPO_ROOT/README.md" "$SRCDIR/README.md"
    mkdir -p "$SRCDIR/doc"
    cp "$REPO_ROOT/doc/JC_Workflow_Specification.md" "$SRCDIR/doc/" 2>/dev/null || true

    # Launcher script (shared across deb/rpm/arch)
    cp "$SCRIPT_DIR/../jarvisagent-launcher.sh" "$SRCDIR/jarvisagent-launcher.sh"

    # rpmbuild validates Source0 exists even with --noprep; create a placeholder
    touch "$RPMBUILD_DIR/SOURCES/${PKG_NAME}-${PKG_VERSION}.tar.gz"

    rpmbuild --define "_topdir $RPMBUILD_DIR" \
             --noclean \
             --nocheck \
             -bb "$RPMBUILD_DIR/SPECS/jarvisagent.spec" \
             --nodeps \
             --noprep \
        || echo "WARNING: rpmbuild failed — this is expected on non-RPM systems"

    # Copy resulting RPM(s) to the top-level build directory
    find "$RPMBUILD_DIR/RPMS" -name '*.rpm' -exec cp {} "$BUILD_DIR/" \;
elif command -v fpm &>/dev/null; then
    echo "==> Building RPM with fpm ..."
    cd "$BUILD_DIR"
    fpm -s dir -t rpm \
        -n "$PKG_NAME" \
        -v "$PKG_VERSION" \
        --iteration "$PKG_RELEASE" \
        -a "$PKG_ARCH" \
        --description "Parallel AI-driven automation with C++ backend and React frontend" \
        --url "https://github.com/beaumanvienna/JarvisAgent" \
        --license "GPL-3.0-only" \
        --depends python3 --depends bash --depends zlib --depends libpq \
        --after-install "$SCRIPT_DIR/postinst.sh" \
        --after-remove "$SCRIPT_DIR/postrm.sh" \
        -C "$STAGING" \
        .
else
    echo "==> Neither rpmbuild nor fpm found."
    echo "    On Fedora/RHEL:  sudo dnf install rpm-build"
    echo "    Universal:  gem install fpm"
    echo ""
    echo "    Package tree assembled in: $STAGING"
    echo "    You can inspect it or transfer to an RPM-based system to build."
fi

echo ""
echo "==> Done. Build output in: $BUILD_DIR"
find "$BUILD_DIR" -maxdepth 1 -name '*.rpm' -exec ls -lh {} + 2>/dev/null || echo "    (no .rpm file — see notes above)"
