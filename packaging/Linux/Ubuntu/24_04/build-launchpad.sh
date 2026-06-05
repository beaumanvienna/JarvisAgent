#!/bin/bash
# build-launchpad.sh — Build a Debian source package for upload to Launchpad PPA.
#
# Usage:
#   cd packaging/Linux/Ubuntu/24_04
#   ./build-launchpad.sh [--no-sign]
#
# Prerequisites:
#   sudo apt install devscripts debhelper dh-make nodejs npm
#   GPG key registered with your Launchpad account (unless --no-sign)
#
# This script:
#   1. Builds the React UIs (npm install + npm run build) — required because
#      Launchpad has no network access during builds, so node_modules can't
#      be fetched. Pre-built dist/ directories are included in the tarball.
#   2. Creates a clean source tree with the debian/ directory.
#   3. Runs debuild -S to produce the .dsc + .tar.xz source package.
#   4. Prints dput instructions for uploading to the PPA.
#
# PPA: ppa:beauman/marley
# premake5 is already published in the same PPA.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
DEBIAN_DIR="$SCRIPT_DIR/debian"

# Sign with this key explicitly. debuild otherwise derives the signing identity
# from the changelog maintainer trailer (-- JC <beaumanvienna@gmail.com>) and
# fails with "No secret key" because the registered key's UID is a different
# email. Same key build-ppa.sh uses.
GPG_KEY_ID="9A427F3969A3439AA6E836475C742F9C0115D544"

SIGN=true
if [[ "${1:-}" == "--no-sign" ]]; then
    SIGN=false
fi

# ---- Extract version ----
PKG_VERSION=$(grep 'JARVIS_AGENT_VERSION' "$REPO_ROOT/premake5.lua" | sed 's/.*\\"\(.*\)\\".*$/\1/')
if [[ -z "$PKG_VERSION" ]]; then
    echo "ERROR: could not extract version from premake5.lua"
    exit 1
fi
PKG_NAME="jarvisagent"
echo "==> Building source package: ${PKG_NAME} ${PKG_VERSION}"

# ---- Build React UIs (Launchpad has no npm) ----
echo "==> Building React dashboard ..."
cd "$REPO_ROOT/code/frontend/dashboard/ui"
npm install
npm run build

echo "==> Building React workflow editor ..."
cd "$REPO_ROOT/code/frontend/workflow-editor/ui"
npm install
npm run build

# ---- Create clean build directory ----
BUILD_DIR="$SCRIPT_DIR/build/launchpad"
rm -rf "$BUILD_DIR"
SOURCE_DIR="$BUILD_DIR/${PKG_NAME}-${PKG_VERSION}"
mkdir -p "$SOURCE_DIR"

echo "==> Copying source tree ..."

# Allowlist source assembly: `git archive` ships ONLY tracked files, so secrets
# (certs/*.pem private keys, .mcp_admin_token, *_keys.json.enc, connections.json)
# and runtime data (queue/, workflows/, log/, .venv/) can NEVER reach the public
# source package.  A tar/rsync + --exclude blocklist fails open — any unlisted
# secret leaks, and the previous list missed the private keys; git archive fails
# closed (gitignored == absent by construction).
git -C "$REPO_ROOT" archive --format=tar HEAD | tar -xf - -C "$SOURCE_DIR"

# git archive omits two gitignored/untracked inputs the Launchpad build needs —
# re-add them explicitly (the only things not under version control):
#   - pre-built React dist/ (gitignored; Launchpad has no reliable npm)
#   - premake-core source (untracked; Launchpad has no network to clone it)
cp -r "$REPO_ROOT/code/frontend/dashboard/ui/dist"       "$SOURCE_DIR/code/frontend/dashboard/ui/dist"
cp -r "$REPO_ROOT/code/frontend/workflow-editor/ui/dist" "$SOURCE_DIR/code/frontend/workflow-editor/ui/dist"

if [[ ! -d "$REPO_ROOT/code/vendor/premake-core" ]]; then
    echo "ERROR: code/vendor/premake-core not found (Launchpad has no network — bundle it first):"
    echo "  git clone --branch v5.0.0-beta8 https://github.com/premake/premake-core code/vendor/premake-core"
    exit 1
fi
rsync -a --exclude='.git/' --exclude='build/' --exclude='bin/' --exclude='obj/' \
    "$REPO_ROOT/code/vendor/premake-core/" "$SOURCE_DIR/code/vendor/premake-core/"

# Ensure pre-built React UIs are in the source tree
if [[ ! -d "$SOURCE_DIR/code/frontend/dashboard/ui/dist" ]]; then
    echo "ERROR: code/frontend/dashboard/ui/dist not found after copy"
    exit 1
fi
if [[ ! -d "$SOURCE_DIR/code/frontend/workflow-editor/ui/dist" ]]; then
    echo "ERROR: code/frontend/workflow-editor/ui/dist not found after copy"
    exit 1
fi

# ---- Copy debian/ directory into source tree ----
echo "==> Adding debian/ directory ..."
cp -r "$DEBIAN_DIR" "$SOURCE_DIR/debian"
chmod +x "$SOURCE_DIR/debian/rules"
chmod +x "$SOURCE_DIR/debian/postinst"
chmod +x "$SOURCE_DIR/debian/postrm"
chmod +x "$SOURCE_DIR/debian/jarvisagent.wrapper"

# Update the TOP changelog stanza's version to match premake5.lua.
# Anchored to line 1 — an unanchored substitution rewrites EVERY historical
# stanza header to the current version, collapsing the version history into N
# identical entries (lintian: latest-debian-changelog-entry-reuses-existing-version).
sed -i "1s/^jarvisagent (.*)/jarvisagent (${PKG_VERSION})/" \
    "$SOURCE_DIR/debian/changelog"

# ---- Build source package ----
echo "==> Running debuild -S ..."
cd "$SOURCE_DIR"

if [[ "$SIGN" == true ]]; then
    debuild -S -sa -k"$GPG_KEY_ID"
else
    debuild -S -sa -us -uc
fi

# ---- Summary ----
echo ""
echo "============================================"
echo "  Source package built successfully!"
echo "============================================"
echo ""
echo "  Files in: $BUILD_DIR/"
ls -lh "$BUILD_DIR/"*.dsc "$BUILD_DIR/"*.tar.* "$BUILD_DIR/"*.changes 2>/dev/null || true
echo ""
echo "  Upload to Launchpad:"
echo "    dput ppa:beauman/marley $BUILD_DIR/${PKG_NAME}_${PKG_VERSION}_source.changes"
echo ""
echo "  Monitor builds at:"
echo "    https://launchpad.net/~beauman/+archive/ubuntu/marley/+packages"
echo ""
