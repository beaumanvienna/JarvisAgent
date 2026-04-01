#!/bin/bash
# build-ppa.sh — Build a signed source package and upload to Launchpad PPA.
#
# Usage:
#   cd packaging/Linux/Ubuntu/24_04
#   ./build-ppa.sh                          # Engine edition, build + upload
#   ./build-ppa.sh --edition studio         # Studio edition, build + upload
#   ./build-ppa.sh --no-upload              # build source package only (skip dput)
#   ./build-ppa.sh --test-build             # also do a local binary test build
#   ./build-ppa.sh --edition studio --no-upload
#
# Prerequisites:
#   sudo apt install devscripts debhelper dput gpg
#   - GPG key registered on https://launchpad.net/~beauman
#   - React UIs already built (dashboard/ui/dist + workflow-editor/ui/dist)
#   - ~/.dput.cf configured (or uses default ppa: target)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
PPA="ppa:beauman/marley"
GPG_KEY_ID="9A427F3969A3439AA6E836475C742F9C0115D544"

# ---- Parse options ----
UPLOAD=true
TEST_BUILD=false
EDITION="engine"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-upload)  UPLOAD=false; shift ;;
        --test-build) TEST_BUILD=true; shift ;;
        --edition)
            if [[ -z "${2:-}" || ("$2" != "engine" && "$2" != "studio") ]]; then
                echo "ERROR: --edition requires 'engine' or 'studio'"
                exit 1
            fi
            EDITION="$2"; shift 2 ;;
        *)            echo "Unknown option: $1"; exit 1 ;;
    esac
done
echo "==> Edition: $EDITION"

# ---- Extract version from premake5.lua ----
PKG_VERSION=$(grep 'JARVIS_AGENT_VERSION' "$REPO_ROOT/premake5.lua" | sed 's/.*\\"\(.*\)\\".*$/\1/')
if [[ -z "$PKG_VERSION" ]]; then
    echo "ERROR: could not extract version from premake5.lua"
    exit 1
fi
echo "==> Version: $PKG_VERSION"

if [[ "$EDITION" == "studio" ]]; then
    PKG_NAME="jarvisagent-studio"
else
    PKG_NAME="jarvisagent"
fi
WORK_DIR="/tmp/${PKG_NAME}-ppa-build"
SRC_DIR="${WORK_DIR}/${PKG_NAME}-${PKG_VERSION}"

# ---- Clean previous build ----
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

# ---- Step 1: Download premake5 source (bundled in tarball; no network on Launchpad) ----
PREMAKE_DIR="$WORK_DIR/premake-core"
if [[ -d "$REPO_ROOT/vendor/premake-core/.git" ]]; then
    echo "==> Using existing vendor/premake-core"
else
    echo "==> Downloading premake5 source ..."
    git clone --depth 1 https://github.com/premake/premake-core "$PREMAKE_DIR"
fi

# ---- Step 2: Verify pre-built React UIs exist ----
for ui in dashboard/ui/dist workflow-editor/ui/dist; do
    if [[ ! -d "$REPO_ROOT/$ui" ]]; then
        echo "ERROR: $ui not found. Build React UIs first:"
        echo "  cd $REPO_ROOT/dashboard/ui && npm install && npm run build"
        echo "  cd $REPO_ROOT/workflow-editor/ui && npx vite build"
        exit 1
    fi
done
echo "==> React UIs found OK"

# ---- Step 3: Export source tree (excluding build artifacts, .git, etc.) ----
echo "==> Exporting source tree to $SRC_DIR ..."
rsync -a --delete \
    --exclude='.git/' \
    --exclude='.github/' \
    --exclude='.vscode/' \
    --exclude='.windsurf/' \
    --exclude='.cache/' \
    --exclude='.venv/' \
    --exclude='.vs/' \
    --exclude='node_modules/' \
    --exclude='bin/' \
    --exclude='bin-int/' \
    --exclude='vendor/curl/bin/' \
    --exclude='vendor/curl/bin-int/' \
    --exclude='vendor/openssl/bin/' \
    --exclude='vendor/openssl/bin-int/' \
    --exclude='vendor/pdcursesmod/bin/' \
    --exclude='vendor/pdcursesmod/bin-int/' \
    --exclude='.flatpak-builder/' \
    --exclude='build/' \
    --exclude='/queue/*' \
    --exclude='/log/*' \
    --exclude='*.o' \
    --exclude='*.a' \
    --exclude='*.d' \
    --exclude='*.pyc' \
    --exclude='__pycache__/' \
    --exclude='*.tsbuildinfo' \
    --exclude='compile_commands.json' \
    --exclude='config.json' \
    --exclude='keys.json' \
    --exclude='keys.json.enc' \
    --exclude='*.sln' \
    --exclude='*.vcxproj' \
    --exclude='*.vcxproj.*' \
    --exclude='.DS_Store' \
    --exclude='vendor/premake-core/' \
    "$REPO_ROOT/" "$SRC_DIR/"

# ---- Step 4: Bundle premake5 source into source tree ----
echo "==> Bundling premake5 source ..."
PREMAKE_SRC="$PREMAKE_DIR"
if [[ -d "$REPO_ROOT/vendor/premake-core/.git" ]]; then
    PREMAKE_SRC="$REPO_ROOT/vendor/premake-core"
fi
rsync -a --delete \
    --exclude='.git/' \
    --exclude='build/' \
    --exclude='bin/' \
    --exclude='obj/' \
    "$PREMAKE_SRC/" "$SRC_DIR/vendor/premake-core/"

# ---- Step 5: Inject pre-built React UIs (gitignored, not in rsync) ----
echo "==> Injecting pre-built React UIs ..."
cp -r "$REPO_ROOT/dashboard/ui/dist"        "$SRC_DIR/dashboard/ui/dist"
cp -r "$REPO_ROOT/workflow-editor/ui/dist"  "$SRC_DIR/workflow-editor/ui/dist"

# ---- Step 6: Copy debian/ directory to source root ----
echo "==> Copying debian/ directory ..."
rm -rf "$SRC_DIR/debian"
cp -r "$SCRIPT_DIR/debian" "$SRC_DIR/debian"

# For Studio edition: patch debian/rules to pass --studio to premake5
if [[ "$EDITION" == "studio" ]]; then
    echo "==> Patching debian/rules for Studio edition ..."
    sed -i 's|vendor/premake-core/bin/release/premake5 gmake$|vendor/premake-core/bin/release/premake5 gmake --studio|' \
        "$SRC_DIR/debian/rules"
fi

# ---- Step 7: Verify changelog version matches ----
CHANGELOG_VER=$(head -1 "$SRC_DIR/debian/changelog" | sed 's/.*(\(.*\)).*/\1/')
if [[ "$CHANGELOG_VER" != "$PKG_VERSION" ]]; then
    echo "WARNING: changelog version ($CHANGELOG_VER) != premake5 version ($PKG_VERSION)"
    echo "         Update debian/changelog before uploading."
fi

# ---- Step 8: Build signed source package ----
echo "==> Building signed source package ..."
cd "$SRC_DIR"
debuild -S -sa -k"$GPG_KEY_ID"

# ---- Step 9: Show results ----
echo ""
echo "==> Source package files:"
ls -lh "${WORK_DIR}/${PKG_NAME}_${PKG_VERSION}"*
echo ""

# ---- Step 10: Optional local test build ----
if [[ "$TEST_BUILD" == true ]]; then
    echo "==> Running local test build (dpkg-buildpackage) ..."
    cd "$SRC_DIR"
    dpkg-buildpackage -us -uc -b
    echo ""
    echo "==> Local test build complete. Binary packages:"
    ls -lh "${WORK_DIR}/${PKG_NAME}_${PKG_VERSION}"*.deb 2>/dev/null || echo "    (no .deb found)"
fi

# ---- Step 11: Upload to PPA ----
CHANGES_FILE="${WORK_DIR}/${PKG_NAME}_${PKG_VERSION}_source.changes"
if [[ "$UPLOAD" == true ]]; then
    if [[ ! -f "$CHANGES_FILE" ]]; then
        echo "ERROR: $CHANGES_FILE not found"
        exit 1
    fi
    echo "==> Uploading to $PPA ..."
    dput "$PPA" "$CHANGES_FILE"
    echo ""
    echo "==> Upload complete!"
    echo "    Monitor build at: https://launchpad.net/~beauman/+archive/ubuntu/marley/+packages"
else
    echo "==> Skipped upload (--no-upload). To upload manually:"
    echo "    dput $PPA $CHANGES_FILE"
fi

echo ""
echo "==> Done."
