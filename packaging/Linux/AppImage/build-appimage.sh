#!/bin/bash
# build-appimage.sh — Build a JarvisAgent AppImage from the current source tree.
#
# Usage:
#   cd packaging/Linux/AppImage
#   ./build-appimage.sh [--dry-run]
#
# Prerequisites:
#   - Build deps: build-essential / base-devel, premake5, nodejs, npm, python3-dev
#   - appimagetool: https://github.com/AppImage/appimagetool/releases
#     (download, chmod +x, place on PATH)
#   - Optional: linuxdeploy for automatic shared lib bundling
#
# Options:
#   --dry-run   Assemble the AppDir using existing build artifacts (skips C++
#               and React builds).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PKG_VERSION=$(grep 'JARVIS_AGENT_VERSION' "$REPO_ROOT/premake5.lua" | sed 's/.*\\"\(.*\)\\".*$/\1/')
if [[ -z "$PKG_VERSION" ]]; then echo "ERROR: could not extract version from premake5.lua"; exit 1; fi
APPDIR="$SCRIPT_DIR/build/JarvisAgent.AppDir"
OUTPUT="$SCRIPT_DIR/build/JarvisAgent-${PKG_VERSION}-x86_64.AppImage"

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=true
    echo "==> DRY RUN: skipping C++ and React builds, using existing artifacts"
fi

# ---- Clean previous build ----
rm -rf "$SCRIPT_DIR/build"
mkdir -p "$APPDIR/usr/share/jarvisagent"

# ---- Build from source (unless dry-run) ----
if [[ "$DRY_RUN" == false ]]; then
    cd "$REPO_ROOT"

    echo "==> Building Engine edition ..."
    premake5 gmake
    make -j"$(nproc)" config=release
    cp bin/Release/jarvisAgent bin/Release/jarvisAgent-engine

    echo "==> Building Studio edition ..."
    premake5 gmake --studio
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

# ---- Assemble AppDir ----
echo "==> Assembling AppDir ..."

SHARE="$APPDIR/usr/share/jarvisagent"

# Binaries (Studio + Engine)
mkdir -p "$SHARE/bin"
for bin in jarvisAgent jarvisAgent-engine; do
    if [[ -f "$REPO_ROOT/bin/Release/$bin" ]]; then
        cp "$REPO_ROOT/bin/Release/$bin" "$SHARE/bin/$bin"
        chmod 755 "$SHARE/bin/$bin"
    else
        echo "WARNING: bin/Release/$bin not found (expected in dry-run)"
    fi
done

# React UIs
if [[ -d "$REPO_ROOT/dashboard/ui/dist" ]]; then
    mkdir -p "$SHARE/dashboard/ui"
    cp -r "$REPO_ROOT/dashboard/ui/dist" "$SHARE/dashboard/ui/dist"
else
    echo "WARNING: dashboard/ui/dist not found (expected in dry-run)"
fi

if [[ -d "$REPO_ROOT/workflow-editor/ui/dist" ]]; then
    mkdir -p "$SHARE/workflow-editor/ui"
    cp -r "$REPO_ROOT/workflow-editor/ui/dist" "$SHARE/workflow-editor/ui/dist"
else
    echo "WARNING: workflow-editor/ui/dist not found (expected in dry-run)"
fi

# Scripts (excluding __pycache__)
cp -r "$REPO_ROOT/scripts" "$SHARE/scripts"
find "$SHARE/scripts" -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
chmod +x "$SHARE/scripts/"*.sh

# Example workflows (curated list — no subdirs, no build artifacts)
mkdir -p "$SHARE/example-workflows"
for jcwf in aiCarMaintenancePipeline aiZipDemo \
            make-example portfolioDividendAnalysis \
            vehicleTroubleshootingGuide; do
    cp "$REPO_ROOT/example/workflows/${jcwf}.jcwf" "$SHARE/example-workflows/" 2>/dev/null || true
done
# Loose input files needed by the example workflows
for f in app.cpp lib1.cpp lib2.cpp main.cpp mylib.h \
         message_engine_question.txt message_tire_question.txt \
         message_unclear_question.txt port62pos.csv; do
    cp "$REPO_ROOT/example/workflows/$f" "$SHARE/example-workflows/" 2>/dev/null || true
done
# Symlink used by aiCarMaintenancePipeline
ln -sf message_engine_question.txt "$SHARE/example-workflows/message.txt"

# Example config
cp "$REPO_ROOT/packaging/config.json.example" "$SHARE/config.json.example"

# Documentation
mkdir -p "$SHARE/doc"
cp "$REPO_ROOT/README.md" "$SHARE/doc/README.md"
cp "$REPO_ROOT/doc/JC_Workflow_Specification.md" "$SHARE/doc/JC_Workflow_Specification.md" 2>/dev/null || true
cp "$REPO_ROOT/doc/jarvisagent.1" "$SHARE/doc/jarvisagent.1" 2>/dev/null || true

# AppRun + desktop + icon
cp "$SCRIPT_DIR/AppRun" "$APPDIR/AppRun"
chmod 755 "$APPDIR/AppRun"

cp "$SCRIPT_DIR/jarvisagent.desktop" "$APPDIR/jarvisagent.desktop"

# Icon — use existing icon or generate a placeholder
if [[ -f "$REPO_ROOT/example/icon.png" ]]; then
    cp "$REPO_ROOT/example/icon.png" "$APPDIR/jarvisagent.png"
elif [[ -f "$REPO_ROOT/example/screenshot.png" ]]; then
    cp "$REPO_ROOT/example/screenshot.png" "$APPDIR/jarvisagent.png"
else
    # Generate a minimal 1x1 PNG placeholder (valid PNG)
    printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\x00\x00\x00\x00IEND\xaeB`\x82' > "$APPDIR/jarvisagent.png"
    echo "NOTE: Using placeholder icon. Replace $APPDIR/jarvisagent.png with a real icon."
fi

# ---- Bundle shared libraries ----
if command -v linuxdeploy &>/dev/null; then
    echo "==> Bundling shared libraries with linuxdeploy ..."
    linuxdeploy --appdir "$APPDIR" \
        --executable "$SHARE/bin/jarvisAgent" \
        --desktop-file "$APPDIR/jarvisagent.desktop" \
        --icon-file "$APPDIR/jarvisagent.png" \
        --output appimage
    # linuxdeploy generates the AppImage directly
    mv JarvisAgent-*.AppImage "$OUTPUT" 2>/dev/null || true
elif command -v appimagetool &>/dev/null; then
    echo "==> Creating AppImage with appimagetool ..."
    echo "    NOTE: shared libs are NOT bundled — the host system must provide them."
    echo "    For fully portable AppImages, install linuxdeploy."
    ARCH=x86_64 appimagetool "$APPDIR" "$OUTPUT"
else
    echo "==> Neither linuxdeploy nor appimagetool found."
    echo "    Install one of:"
    echo "      - appimagetool: https://github.com/AppImage/appimagetool/releases"
    echo "      - linuxdeploy:  https://github.com/linuxdeploy/linuxdeploy/releases"
    echo ""
    echo "    AppDir assembled at: $APPDIR"
    echo "    You can manually run: ARCH=x86_64 appimagetool $APPDIR $OUTPUT"
    exit 0
fi

chmod +x "$OUTPUT"

echo ""
echo "==> AppImage created: $OUTPUT"
echo "    Size: $(du -h "$OUTPUT" | cut -f1)"
echo ""
echo "    Run it:"
echo "      ./$OUTPUT"
echo ""
echo "    Data directory: ~/.local/share/jarvisagent/"
echo "    Config:         ~/.local/share/jarvisagent/config.json"
echo "    Override data dir with: JARVISAGENT_DATA=/path/to/dir ./JarvisAgent-x86_64.AppImage"
