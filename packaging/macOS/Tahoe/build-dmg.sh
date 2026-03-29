#!/bin/bash
# build-dmg.sh — Build a .dmg installer for JarvisAgent on macOS.
#
# Usage:
#   cd packaging/macOS/Tahoe
#   ./build-dmg.sh [--dry-run]
#
# Prerequisites:
#   - Xcode Command Line Tools: xcode-select --install
#   - Homebrew: brew install premake node python@3
#
# Options:
#   --dry-run   Assemble the .app bundle using existing build artifacts
#               (skips C++ and React builds).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PKG_VERSION=$(grep 'JARVIS_AGENT_VERSION' "$REPO_ROOT/premake5.lua" | sed 's/.*\\"\(.*\)\\".*$/\1/')
if [[ -z "$PKG_VERSION" ]]; then echo "ERROR: could not extract version from premake5.lua"; exit 1; fi
BUILD_DIR="$SCRIPT_DIR/build"
APP_NAME="JarvisAgent"
APP_BUNDLE="$BUILD_DIR/${APP_NAME}.app"
DMG_NAME="${APP_NAME}-${PKG_VERSION}.dmg"

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=true
    echo "==> DRY RUN: skipping C++ and React builds, using existing artifacts"
fi

# ---- Clean previous build ----
rm -rf "$BUILD_DIR"
mkdir -p "$APP_BUNDLE/Contents/MacOS"
mkdir -p "$APP_BUNDLE/Contents/Resources/share"

# ---- Build from source (unless dry-run) ----
if [[ "$DRY_RUN" == false ]]; then
    echo "==> Generating Makefiles ..."
    cd "$REPO_ROOT"
    premake5 gmake

    CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

    echo "==> Building C++ release binary ($CORES cores) ..."
    make -j"$CORES" config=release

    echo "==> Building React dashboard ..."
    cd "$REPO_ROOT/dashboard/ui"
    npm install
    npm run build

    echo "==> Building React workflow editor ..."
    cd "$REPO_ROOT/workflow-editor/ui"
    npm install
    npm run build
fi

# ---- Assemble .app bundle ----
echo "==> Assembling ${APP_NAME}.app ..."

SHARE="$APP_BUNDLE/Contents/Resources/share"

# Binary
if [[ -f "$REPO_ROOT/bin/Release/jarvisAgent" ]]; then
    cp "$REPO_ROOT/bin/Release/jarvisAgent" "$SHARE/jarvisAgent"
    chmod 755 "$SHARE/jarvisAgent"
else
    echo "WARNING: bin/Release/jarvisAgent not found (expected in dry-run)"
fi

# React UIs
if [[ -d "$REPO_ROOT/dashboard/ui/dist" ]]; then
    mkdir -p "$SHARE/dashboard/ui"
    cp -r "$REPO_ROOT/dashboard/ui/dist" "$SHARE/dashboard/ui/dist"
fi

if [[ -d "$REPO_ROOT/workflow-editor/ui/dist" ]]; then
    mkdir -p "$SHARE/workflow-editor/ui"
    cp -r "$REPO_ROOT/workflow-editor/ui/dist" "$SHARE/workflow-editor/ui/dist"
fi

# Scripts (excluding __pycache__)
cp -r "$REPO_ROOT/scripts" "$SHARE/scripts"
find "$SHARE/scripts" -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
chmod +x "$SHARE/scripts/"*.sh

# Example workflows (curated list — no subdirs, no build artifacts)
mkdir -p "$SHARE/example-workflows"
for jcwf in aiCarMaintenancePipeline aiZipDemo \
            make-example portfolioDividendAnalysis; do
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

# Patch Homebrew formula with current version
cp "$SCRIPT_DIR/jarvisagent.rb" "$BUILD_DIR/jarvisagent.rb"
sed -i '' "s/^  version \".*\"/  version \"$PKG_VERSION\"/" "$BUILD_DIR/jarvisagent.rb"

# ---- Launcher script (Contents/MacOS/JarvisAgent) ----
cat > "$APP_BUNDLE/Contents/MacOS/JarvisAgent" <<'LAUNCHER'
#!/bin/bash
# JarvisAgent.app launcher — user-space setup for macOS .app bundle.
# Creates a per-user working directory with symlinks to read-only assets
# inside the .app bundle and writable directories for user data.

set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SHARE="$APP_DIR/Resources/share"
OPEN_BROWSER=true
USER_HOME=""

# ---- Parse launcher-specific arguments ----
PASSTHROUGH_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --home)
            if [[ -z "${2:-}" ]]; then
                echo "Error: --home requires a directory argument"
                exit 1
            fi
            USER_HOME="$2"
            shift 2
            ;;
        --no-browser)
            OPEN_BROWSER=false
            shift
            ;;
        --help|-h|--version|-v)
            exec "$SHARE/jarvisAgent" "$1"
            ;;
        *)
            PASSTHROUGH_ARGS+=("$1")
            shift
            ;;
    esac
done

# ---- Determine working directory ----
if [[ -z "$USER_HOME" ]]; then
    USER_HOME="${JARVISAGENT_HOME:-$HOME/JarvisAgent}"
fi

# ---- First-run setup ----
if [[ ! -d "$USER_HOME" ]]; then
    echo "==> First run: creating working directory at $USER_HOME"
    mkdir -p "$USER_HOME"
fi

# Symlink read-only assets (ln -sfn: atomic, no rm needed)
for asset in dashboard workflow-editor scripts doc; do
    if [[ -L "$USER_HOME/$asset" ]] || [[ ! -e "$USER_HOME/$asset" ]]; then
        ln -sfn "$SHARE/$asset" "$USER_HOME/$asset"
    fi
done

# Binary symlink
mkdir -p "$USER_HOME/bin"
if [[ -L "$USER_HOME/bin/jarvisAgent" ]] || [[ ! -e "$USER_HOME/bin/jarvisAgent" ]]; then
    ln -sfn "$SHARE/jarvisAgent" "$USER_HOME/bin/jarvisAgent"
fi

# Writable directories
mkdir -p "$USER_HOME/queue"
mkdir -p "$USER_HOME/log"
mkdir -p "$USER_HOME/workflows"

# Copy example workflows on first run (only if workflows dir is empty)
if [[ -d "$SHARE/example-workflows" ]] && [[ -z "$(ls -A "$USER_HOME/workflows" 2>/dev/null)" ]]; then
    cp -a "$SHARE/example-workflows/"* "$USER_HOME/workflows/" 2>/dev/null || true
    echo "==> Copied example workflows to $USER_HOME/workflows/"
fi

# Copy example config on first run
if [[ ! -f "$USER_HOME/config.json" ]]; then
    if [[ -f "$SHARE/config.json.example" ]]; then
        cp "$SHARE/config.json.example" "$USER_HOME/config.json"
        echo "==> Created $USER_HOME/config.json from example"
        echo "    Please edit it to set your API keys."
    fi
fi

# ---- Python venv (first-run) ----
if [[ ! -d "$USER_HOME/.venv" ]]; then
    echo "==> Creating Python virtual environment at $USER_HOME/.venv ..."
    python3 -m venv "$USER_HOME/.venv" 2>/dev/null || {
        echo "WARNING: Could not create Python venv"
        echo "         Shell tasks using markitdown/md2pdf will not work until fixed."
    }

    if [[ -d "$USER_HOME/.venv" ]]; then
        "$USER_HOME/.venv/bin/pip" install --quiet --upgrade pip 2>/dev/null || true
        echo "==> Installing Python tools (markitdown) ..."
        "$USER_HOME/.venv/bin/pip" install --quiet "markitdown[all]" 2>/dev/null || true
    fi
fi

# Activate venv if present
if [[ -f "$USER_HOME/.venv/bin/activate" ]]; then
    source "$USER_HOME/.venv/bin/activate"
fi

# ---- Open dashboard in default browser ----
if [[ "$OPEN_BROWSER" == true ]]; then
    (sleep 2 && open "http://localhost:8080" 2>/dev/null) &
fi

# ---- Launch ----
echo "==> Starting JarvisAgent in $USER_HOME"
echo "    Dashboard: http://localhost:8080"
echo "    Editor:    http://localhost:8080/editor"
echo ""
cd "$USER_HOME"
exec "$USER_HOME/bin/jarvisAgent" "${PASSTHROUGH_ARGS[@]}"
LAUNCHER
chmod 755 "$APP_BUNDLE/Contents/MacOS/JarvisAgent"

# ---- Info.plist ----
cat > "$APP_BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>JarvisAgent</string>
    <key>CFBundleDisplayName</key>
    <string>JarvisAgent</string>
    <key>CFBundleIdentifier</key>
    <string>com.jctechnolabs.JarvisAgent</string>
    <key>CFBundleVersion</key>
    <string>${PKG_VERSION}</string>
    <key>CFBundleShortVersionString</key>
    <string>${PKG_VERSION}</string>
    <key>CFBundleExecutable</key>
    <string>JarvisAgent</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST

# ---- Create .dmg ----
echo "==> Creating .dmg ..."
if command -v hdiutil &>/dev/null; then
    hdiutil create -volname "$APP_NAME" \
        -srcfolder "$APP_BUNDLE" \
        -ov -format UDZO \
        "$BUILD_DIR/$DMG_NAME"

    echo ""
    echo "==> DMG created: $BUILD_DIR/$DMG_NAME"
    echo "    Size: $(du -h "$BUILD_DIR/$DMG_NAME" | cut -f1)"
    echo ""
    echo "    Install: open the .dmg and drag JarvisAgent.app to /Applications"
    echo "    Run:     open /Applications/JarvisAgent.app (or from Terminal)"
    echo "    Data:    ~/Library/Application Support/JarvisAgent/"
else
    echo "==> hdiutil not available (not on macOS)."
    echo "    .app bundle assembled at: $APP_BUNDLE"
    echo "    Transfer to macOS and run: hdiutil create -volname JarvisAgent -srcfolder JarvisAgent.app -ov -format UDZO JarvisAgent.dmg"
fi
