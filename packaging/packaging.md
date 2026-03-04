# JarvisAgent — Packaging Plan

Last updated: 2026-03-01

JarvisAgent is a cross-platform C++ application with React frontends. All central
C++ libraries are vendored under `vendor/` so that every platform builds against the
exact same revision — no system-package roulette. Minor platform-specific exceptions
apply (e.g. Schannel instead of OpenSSL on Windows). The build system is
**premake5** (not CMake); on Windows we generate Visual Studio 2022 solutions, on
Linux and macOS we generate GNU Makefiles. MSYS2 with GCC or Clang could be tested
by a volunteer if somebody has the time. The React/npm toolchain is inherently
cross-platform and identical everywhere.

```
Packaging Targets
├── Linux
│   ├── Arch / Manjaro        PKGBUILD  →  .pkg.tar.zst
│   ├── Ubuntu 24.04          build-deb.sh  →  .deb
│   ├── RPM (Fedora/RHEL/Rocky) build-rpm.sh  →  .rpm
│   ├── AppImage              build-appimage.sh  →  .AppImage
│   └── Flatpak               build-flatpak.sh  →  .flatpak
├── macOS Tahoe
│   ├── Homebrew              jarvisagent.rb
│   └── DMG                   build-dmg.sh  →  .dmg
├── Windows 11
│   ├── Portable ZIP          build-zip.ps1  →  .zip
│   └── MSI Installer         build-msi.ps1  →  .msi
└── Docker (all platforms)
    └── OCI image             docker-publish.yml  →  ghcr.io
```

---

## Packaging Decisions

- **Build from source:** Packages compile C++ and build React UIs from source (fully transparent open-source).
- **Package contents:** Release binary (`bin/Release/jarvisAgent`), React UI dist/ folders, scripts, example workflows, example config. No debug binaries or source code.
- **premake5:** Assumed already installed (like make, g++, or the package manager itself). Listed as `makedepends` but not bundled.
- **Python venv:** Created in post-install hook with `markitdown`, `md2pdf-mermaid`, and `playwright`.
- **Install location:** `/opt/jarvisagent/` (self-contained, CWD-relative binary). Launcher script in `/usr/bin/jarvisagent`.
- **Runtime model:** JarvisAgent resolves `config.json`, `dashboard/ui/dist/`, `workflow-editor/ui/dist/`, `scripts/`, `queue/`, `workflows/` relative to CWD. The launcher script cd's to `/opt/jarvisagent/`.

---

## Common Build Requirements

All platforms share the same core build pipeline:

1. **premake5 gmake** (or `vs2022` / `xcode4`) — generates Makefiles / project files
2. **make config=release && make config=debug** — compiles C++ backend
3. **npm install && npm run build** — builds React UIs (dashboard + workflow editor)
4. **Python venv** — `markitdown`, `md2pdf-mermaid`, `playwright` installed via pip

### Vendored Libraries (built from source, no system packages needed)

asio, crow, curl, openssl, date, pdcursesmod, python, simdjson, spdlog, thread-pool, tracy

All vendored in `vendor/`. OpenSSL and libcurl are intentionally vendored to avoid version conflicts across distros.

**Platform-specific TLS backends:**
- **Linux / macOS** — libcurl uses vendored OpenSSL (`ssl` + `crypto`) for HTTPS.
- **Windows** — libcurl uses **Schannel** (native Windows TLS via `USE_SCHANNEL` / `USE_WINDOWS_SSPI`).
  OpenSSL is still linked on Windows for build compatibility but Schannel handles all HTTPS traffic.

### Build-time Dependencies

| Dependency | Purpose |
|------------|---------|
| C++ compiler (GCC / Clang / MSVC) | Backend compilation |
| make | Build system |
| premake5 | Generates Makefiles from `premake5.lua` |
| Python 3 + dev headers | Embedded Python support |
| zlib (dev) | Compression (linked at build time) |
| Node.js + npm | React UI builds |

### Runtime Dependencies

| Dependency | Purpose |
|------------|---------|
| Python 3 | Python task execution, markitdown, md2pdf |
| zlib | Compression |
| bash | Shell task execution |
| Chromium (headless) | playwright for md2pdf-mermaid |

### Runtime Python Tools (installed in venv)

| Tool | Purpose |
|------|---------|
| markitdown | Office document → Markdown conversion |
| md2pdf-mermaid | Markdown → PDF with Mermaid diagrams |
| playwright | Headless Chrome for md2pdf-mermaid |

---

## Linux

### Arch (Manjaro, EndeavourOS, etc.)

**Format:** PKGBUILD → pacman package (`.pkg.tar.zst`)
**Directory:** `packaging/Linux/Arch/`
**Status:** PKGBUILD created

**Package names (Arch):**

| Build dep | Arch package |
|-----------|-------------|
| base toolchain | `base-devel` (meta-package) |
| Python 3 + headers | `python` |
| zlib | `zlib` |
| premake5 | AUR: `premake` |
| Node.js + npm | `nodejs` `npm` |
| git | `git` |

| Runtime dep | Arch package |
|-------------|-------------|
| Python 3 | `python` |
| zlib | `zlib` |
| bash | `bash` (part of `base`) |
| pip | `python-pip` |

**Files:**
- `PKGBUILD` — package build script
- `jarvisagent.install` — post-install hook (creates venv, installs pip tools + playwright)

**Build & install:**
```bash
# From packaging/Linux/Arch/
makepkg -si

# Or via AUR helper (yay, paru, etc.):
yay -S jarvisagent-git
```

**After install:**
```bash
sudo cp /opt/jarvisagent/config.json.example /opt/jarvisagent/config.json
# Edit config.json (API keys, queue/workflow paths)
source /opt/jarvisagent/.venv/bin/activate
jarvisagent
```

**Uninstall:**
```bash
sudo pacman -R jarvisagent-git
```

---

### Ubuntu 24.04

**Format:** `.deb` (via `dpkg-deb` or `debhelper`)
**Directory:** `packaging/Linux/Ubuntu/24_04/`
**Status:** Not started

**Package names (Ubuntu/Debian):**

| Build dep | Ubuntu package |
|-----------|---------------|
| Toolchain | `build-essential` |
| Python 3 + headers | `python3` `python3-dev` `python3-pip` `python3-venv` |
| zlib | `zlib1g-dev` |
| SSL (vendored) | — |
| premake5 | Not in repos — download or build from source |
| Node.js + npm | `nodejs` `npm` |

| Runtime dep | Ubuntu package |
|-------------|---------------|
| Python 3 | `python3` `python3-pip` `python3-venv` |
| zlib | `zlib1g` |
| bash | `bash` |

**Notes:**
- GitHub CI (`linux-workflow.yml`) already validates this dep set on `ubuntu-latest`.
- premake5 is not in official repos — download binary or build from source.

**Install (planned):**
```bash
sudo dpkg -i jarvisagent_0.1-1_amd64.deb
sudo apt install -f   # resolve dependencies
```

**Uninstall:**
```bash
sudo apt remove jarvisagent
```

---

### RPM (Fedora / RHEL / Rocky / CentOS)

**Format:** `.rpm` (via `rpmbuild` / `fpm`)
**Directory:** `packaging/Linux/RPM/`
**Status:** Spec file + build script created

**Package names (Fedora):**

| Build dep | RPM package |
|-----------|---------------|
| Toolchain | `gcc` `gcc-c++` `make` |
| Python 3 + headers | `python3` `python3-devel` `python3-pip` |
| zlib | `zlib-devel` |
| premake5 | Not in repos — download or build from source |
| Node.js + npm | `nodejs` `npm` |

| Runtime dep | RPM package |
|-------------|---------------|
| Python 3 | `python3` `python3-pip` |
| zlib | `zlib` |
| bash | `bash` |

**Files:**
- `jarvisagent.spec` — RPM spec file (for `rpmbuild`)
- `build-rpm.sh` — build script (supports `rpmbuild` and `fpm` fallback, `--dry-run` option)
- `postinst.sh` / `postrm.sh` — post-install/remove hooks (for `fpm`)

**Build & install (Fedora/RHEL/Rocky):**
```bash
# From packaging/Linux/RPM/
chmod +x build-rpm.sh
./build-rpm.sh
sudo dnf install build/jarvisagent-0.1-1.x86_64.rpm
```

**Uninstall:**
```bash
sudo dnf remove jarvisagent
```

---

### AppImage

**Format:** Self-contained `.AppImage` bundle
**Directory:** `packaging/Linux/AppImage/`
**Status:** Build script + AppRun created

**Approach:**
- Bundle the release binary, React UI dist/ assets, and scripts into an AppDir.
- Use `linuxdeploy` (preferred, bundles shared libs) or `appimagetool` to generate the AppImage.
- On first launch, `AppRun` creates `~/.local/share/jarvisagent/` with symlinks to read-only assets and real directories for writable data (`queue/`, `log/`, `workflows/`).
- Python venv is created on first run in the user's data directory.
- Override data dir with `JARVISAGENT_DATA=/path/to/dir`.

**Files:**
- `AppRun` — entry point script (symlinks assets, creates venv on first run)
- `jarvisagent.desktop` — FreeDesktop desktop entry
- `build-appimage.sh` — build script (`--dry-run` option, supports linuxdeploy and appimagetool)

**Build:**
```bash
# From packaging/Linux/AppImage/
chmod +x build-appimage.sh
./build-appimage.sh
```

**Install & run:**
```bash
chmod +x build/JarvisAgent-x86_64.AppImage
./build/JarvisAgent-x86_64.AppImage
```

**Remove:**
```bash
rm JarvisAgent-x86_64.AppImage
rm -rf ~/.local/share/jarvisagent/   # remove data directory
```

---

### Flatpak

**Format:** Flatpak bundle (`.flatpak`)
**Directory:** `packaging/Linux/Flatpak/`
**Status:** Manifest + wrapper + build script created

**Approach:**
- Flatpak manifest (`com.jctechnolabs.JarvisAgent.yml`) based on `org.freedesktop.Sdk//24.08`.
- Builds premake5 from source inside the Flatpak build, then builds JarvisAgent C++ + React UIs.
- Wrapper script creates `~/.local/share/jarvisagent/` with symlinks to read-only assets and writable dirs.
- Python venv created on first run in user data directory.
- `--share=network` for AI API calls, `--filesystem=home` for queue/workflows/log.

**Files:**
- `com.jctechnolabs.JarvisAgent.yml` — Flatpak manifest
- `jarvisagent-wrapper.sh` — entry point (symlinks assets, creates venv on first run)
- `com.jctechnolabs.JarvisAgent.desktop` — FreeDesktop desktop entry
- `build-flatpak.sh` — build script (uses `flatpak-builder`, outputs single-file bundle)

**Prerequisites:**
```bash
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install flathub org.freedesktop.Sdk//24.08 org.freedesktop.Sdk.Extension.node20//24.08
```

**Build:**
```bash
# From packaging/Linux/Flatpak/
chmod +x build-flatpak.sh
./build-flatpak.sh
```

**Install & run:**
```bash
flatpak install build/JarvisAgent.flatpak
flatpak run com.jctechnolabs.JarvisAgent
```

**Uninstall:**
```bash
flatpak uninstall com.jctechnolabs.JarvisAgent
```

**Data directory:** `~/.local/share/jarvisagent/` (config, queue, log, workflows)

---

## macOS

### macOS Tahoe (and later)

**Format:** `.dmg` installer and Homebrew formula
**Directory:** `packaging/macOS/Tahoe/`
**Status:** Homebrew formula + .dmg build script created

**Build tool:** `premake5 gmake` + make (CI uses gmake, not Xcode)

**Package names (Homebrew):**

| Build dep | Homebrew formula |
|-----------|-----------------|
| Xcode CLI tools | `xcode-select --install` |
| Python 3 | `python@3` |
| premake5 | `premake` (Homebrew has it) |
| Node.js | `node` |

| Runtime dep | Notes |
|-------------|-------|
| Python 3 | `python@3` via Homebrew |
| bash | Ships with macOS (v3 is fine for launcher) |

**Notes:**
- Apple Clang's libc++ lacks C++20 chrono timezone — mitigated via vendored `date` library.
- GitHub CI (`macos-workflow.yml`) validates macOS builds.

**Files:**
- `jarvisagent.rb` — Homebrew formula (builds from source, creates venv in post_install)
- `build-dmg.sh` — .dmg build script (`--dry-run` option, creates .app bundle + Info.plist)

**Homebrew install:**
```bash
brew tap beaumanvienna/jarvisagent
brew install jarvisagent
```

**.dmg build & install:**
```bash
# From packaging/macOS/Tahoe/
chmod +x build-dmg.sh
./build-dmg.sh
# Open .dmg, drag JarvisAgent.app to /Applications
```

**Uninstall:**
```bash
# Homebrew
brew uninstall jarvisagent

# .dmg
rm -rf /Applications/JarvisAgent.app
rm -rf ~/Library/Application\ Support/JarvisAgent/
```

**Data directory:**
- Homebrew: `$(brew --prefix)/Cellar/jarvisagent/0.1/`
- .dmg: `~/Library/Application Support/JarvisAgent/`

---

## Windows

### Windows 11

**Format:** Portable `.zip` and MSI installer (via WiX)
**Directory:** `packaging/Windows/11/`
**Status:** Build scripts + WiX manifest created

**Build tool:** Visual Studio 2022 (via `premake5 vs2022` + MSBuild)

**Prerequisites:**
- Visual Studio 2022 with C++ workload (MSBuild on PATH)
- premake5 on PATH
- Python 3 on PATH
- Node.js + npm on PATH
- MSYS2 or Git Bash (required at runtime for shell tasks)
- WiX Toolset v3 or v4 (for MSI only)

**Notes:**
- Shell tasks require `bash` on PATH — MSYS2 or Git Bash must be installed.
- libcurl uses **Schannel** for HTTPS on Windows (`USE_SCHANNEL` / `USE_WINDOWS_SSPI`).
- The Docker workflow (`docker-publish.yml`) provides an alternative deployment path via WSL2.

**Files:**
- `build-zip.ps1` — builds from source + creates portable `.zip` (`-DryRun` option)
- `build-msi.ps1` — creates MSI from staged directory (requires WiX, run after build-zip)
- `jarvisagent.wxs` — WiX v4 manifest for MSI installer

**Portable .zip build & install:**
```powershell
# From packaging\Windows\11\
.\build-zip.ps1
# Extract build\JarvisAgent-x64.zip
# copy config.json.example config.json
# Run setup-venv.bat (one-time)
# Run jarvisagent.bat
```

**MSI build & install:**
```powershell
.\build-zip.ps1          # stage the package tree first
.\build-msi.ps1          # build the MSI
# Double-click build\JarvisAgent-x64.msi
```

**Uninstall:**
```
# MSI: Settings → Apps → JarvisAgent → Uninstall
# Portable: delete the extracted folder
```

---

## Docker (all platforms)

**Format:** OCI container image on GHCR
**Status:** Implemented (`docker-publish.yml`)

The Docker image is already built and pushed to `ghcr.io` on every push to main/master/develop. This is the simplest cross-platform deployment — users with Docker can pull and run without any local build.

**Install:**
```bash
docker pull ghcr.io/beaumanvienna/jarvisagent:latest
docker run -it --rm -p 8080:8080 ghcr.io/beaumanvienna/jarvisagent:latest
```

**Remove:**
```bash
docker rmi ghcr.io/beaumanvienna/jarvisagent:latest
```

---

## Priority Order

1. **Arch** (Manjaro) — current dev machine, test immediately
2. **Ubuntu 24.04** — most common Linux desktop/server, CI already validates
3. **AppImage** — universal Linux, no root required
4. **RPM** — Fedora/RHEL/Rocky/CentOS ecosystem coverage
5. **Flatpak** — sandboxed distribution
6. **macOS Tahoe** — Homebrew tap + .dmg
7. **Windows 11** — MSI/MSIX installer
