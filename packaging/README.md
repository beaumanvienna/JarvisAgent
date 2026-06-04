# JarvisAgent — Packaging Plan

Last updated: 2026-04-10

JarvisAgent is a cross-platform C++ application with React frontends. All central
C++ libraries are vendored under `code/vendor/` so that every platform builds against the
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
- **Python venv:** Created per-user by the launcher on first run (in `~/JarvisAgent/.venv`).
- **Install location:** `/opt/jarvisagent/` (read-only system assets). Launcher script in `/usr/bin/jarvisagent`.
- **Runtime model:** The launcher creates a per-user working directory (`~/JarvisAgent` by default) with symlinks to read-only assets in `/opt/jarvisagent/` and writable directories for user data. JarvisAgent resolves `config.json`, `code/frontend/dashboard/ui/dist/`, `code/frontend/workflow-editor/ui/dist/`, `scripts/`, `queue/`, `workflows/` relative to CWD.
- **Shared launcher:** `packaging/Linux/jarvisagent-launcher.sh` is shared across DEB, RPM, and Arch packages. Supports `--home DIR` (custom working directory), `--no-browser` (skip browser launch), and `JARVISAGENT_HOME` env var.

---

## Common Build Requirements

All platforms share the same core build pipeline:

1. **premake5 gmake** (or `vs2022` / `xcode4`) — generates Makefiles / project files
2. **make config=release && make config=debug** — compiles C++ backend
3. **npm install && npm run build** — builds React UIs (dashboard + workflow editor)
4. **Python venv** — `markitdown` installed via pip; `pandoc` (system) + `mmdc` (npm) for PDF workflows

### Vendored Libraries (built from source, no system packages needed)

asio, crow, curl, openssl, date, pdcursesmod, python, simdjson, spdlog, thread-pool, tracy

All vendored in `code/vendor/`. OpenSSL and libcurl are intentionally vendored to avoid version conflicts across distros.

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
| libpq (dev) | PostgreSQL C client library (cloud integration) |
| Node.js + npm | React UI builds |

### Runtime Dependencies

| Dependency | Purpose |
|------------|---------|
| Python 3 | Python task execution, markitdown |
| zlib | Compression |
| libpq | PostgreSQL client library (cloud integration) |
| bash | Shell task execution |
| pandoc + pdflatex | Markdown → PDF (PDF workflows only) |
| mmdc (@mermaid-js/mermaid-cli) | Mermaid → PNG rendering (PDF workflows only) |

### Runtime Python Tools (installed in venv)

| Tool | Purpose |
|------|---------|
| markitdown | Office document → Markdown conversion |

### Runtime System Tools (installed separately — PDF workflows only)

| Tool | Install | Purpose |
|------|---------|---------|
| pandoc | `apt install pandoc texlive-latex-base texlive-latex-extra` | Markdown → PDF conversion |
| mmdc | `npm install -g @mermaid-js/mermaid-cli@10.x` | Mermaid diagram → PNG rendering |

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
| libpq (PostgreSQL) | `postgresql-libs` |
| premake5 | AUR: `premake` |
| Node.js + npm | `nodejs` `npm` |
| git | `git` |

| Runtime dep | Arch package |
|-------------|-------------|
| Python 3 | `python` |
| zlib | `zlib` |
| libpq (PostgreSQL) | `postgresql-libs` |
| bash | `bash` (part of `base`) |
| pip | `python-pip` |

**Files:**
- `PKGBUILD` — package build script
- `jarvisagent.install` — post-install/remove hooks (prints launcher instructions)
- `prerequisites.sh` — installs all build + runtime dependencies

**Build & install:**
```bash
# From packaging/Linux/Arch/
sudo ./prerequisites.sh   # one-time: install deps
makepkg -si

# Or via AUR helper (yay, paru, etc.):
yay -S jarvisagent-git
```

**After install:**
```bash
jarvisagent                          # creates ~/JarvisAgent, sets up venv, opens browser
jarvisagent --home /path/to/dir      # custom working directory
jarvisagent --no-browser             # skip browser launch
```

**Uninstall:**
```bash
sudo pacman -R jarvisagent-git
rm -rf ~/JarvisAgent/   # remove user data
```

---

### Ubuntu 24.04

**Format:** `.deb` (binary via `dpkg-deb`, source via `debuild` for Launchpad)
**Directory:** `packaging/Linux/Ubuntu/24_04/`
**Status:** Binary .deb built locally via `build-deb.sh`; source package published on Launchpad PPA

**Package names (Ubuntu/Debian):**

| Build dep | Ubuntu package |
|-----------|---------------|
| Toolchain | `build-essential` |
| Python 3 + headers | `python3` `python3-dev` `python3-pip` `python3-venv` |
| zlib | `zlib1g-dev` |
| libpq (PostgreSQL) | `libpq-dev` |
| SSL (vendored) | — |
| premake5 | `ppa:beauman/marley` (v5.0.16.2) |
| Node.js + npm | `nodejs` `npm` |

| Runtime dep | Ubuntu package |
|-------------|---------------|
| Python 3 | `python3` `python3-pip` `python3-venv` |
| zlib | `zlib1g` |
| libpq (PostgreSQL) | `libpq5` |
| bash | `bash` |

**Notes:**
- GitHub CI (`linux-workflow.yml`) already validates this dep set on `ubuntu-latest`.
- premake5 is available from the PPA `ppa:beauman/marley`.

**Files:**
- `build-deb.sh` — build script (`--dry-run` option)
- `DEBIAN/control` — package metadata
- `DEBIAN/postinst` / `DEBIAN/postrm` — post-install/remove hooks (prints launcher instructions)
- `prerequisites.sh` — installs all build + runtime dependencies

**Build & install:**
```bash
# From packaging/Linux/Ubuntu/24_04/
sudo ./prerequisites.sh   # one-time: install deps
./build-deb.sh
sudo dpkg -i build/jarvisagent_*_amd64.deb
sudo apt install -f   # resolve dependencies
jarvisagent            # creates ~/JarvisAgent, sets up venv, opens browser
```

**Install (binary .deb, pre-built):**
```bash
sudo dpkg -i jarvisagent_*_amd64.deb
sudo apt install -f   # resolve dependencies
```

**Install (from PPA):**
```bash
sudo add-apt-repository ppa:beauman/marley
sudo apt update
sudo apt install jarvisagent
```

**Uninstall:**
```bash
sudo apt remove jarvisagent
rm -rf ~/JarvisAgent/   # remove user data
```

---

### Launchpad Source Package (PPA)

**PPA:** `ppa:beauman/marley` — `sudo add-apt-repository ppa:beauman/marley`
**Target series:** Noble (24.04), Jammy (22.04), and others as needed
**premake5:** Already published in the same PPA (v5.0.16.2)

Launchpad builds `.deb` packages from **source packages** on its own build farm.
We upload only the source — Launchpad compiles and publishes the binary `.deb`.

#### Prerequisites

```bash
sudo apt install devscripts debhelper dh-make dput gpg
```

You need a GPG key registered with your Launchpad account for signing.

#### Directory structure

The source package uses a `debian/` directory (lowercase, not `DEBIAN/`) with
debhelper build rules:

```
packaging/Linux/Ubuntu/24_04/debian/
├── changelog          # version, target series, maintainer
├── compat             # debhelper compat level
├── control            # source + binary package metadata
├── copyright          # DEP-5 machine-readable copyright
├── install            # list of files to install
├── postinst           # post-install hook (prints launcher instructions)
├── postrm             # post-remove hook (prints user-data cleanup hint)
├── rules              # makefile — the actual build recipe
└── source/
    └── format         # "3.0 (native)"
```

#### Building the source package

```bash
# 1. Create the source tarball
cd /path/to/JarvisAgent
PKG_VERSION=$(grep 'JARVIS_AGENT_VERSION' premake5.lua | sed 's/.*\\"\(.*\)\\".*$/\1/')

# 2. Create orig tarball (exclude .git, node_modules, build artifacts)
tar czf ../jarvisagent_${PKG_VERSION}.orig.tar.gz \
    --exclude='.git' \
    --exclude='node_modules' \
    --exclude='bin' \
    --exclude='bin-int' \
    --transform "s,^\.,jarvisagent-${PKG_VERSION}," \
    .

# 3. Build the source package (signed with your GPG key)
debuild -S -sa

# This produces in the parent directory:
#   jarvisagent_<version>.dsc
#   jarvisagent_<version>.orig.tar.gz
#   jarvisagent_<version>.debian.tar.xz
#   jarvisagent_<version>_source.changes
```

#### Uploading to Launchpad

```bash
# Upload to PPA (uses the .changes file produced by debuild)
dput ppa:beauman/marley ../jarvisagent_${PKG_VERSION}_source.changes
```

After upload, Launchpad will:
1. Accept the source package (check GPG signature)
2. Queue a build for each target architecture (amd64, etc.)
3. Publish the binary `.deb` in the PPA once the build succeeds

Monitor build status at: https://launchpad.net/~beauman/+archive/ubuntu/marley/+packages

#### Targeting multiple Ubuntu series

To publish for multiple series (e.g. Noble + Jammy), rebuild the source package
with a different `debian/changelog` entry for each series:

```bash
# Edit debian/changelog — change "noble" to "jammy", bump version suffix
dch -i -D jammy "Backport to Jammy"
debuild -S -sd          # -sd = don't include orig tarball (already uploaded)
dput ppa:beauman/marley ../jarvisagent_*jammy*_source.changes
```

#### Key points

- **premake5 is bundled:** `build-ppa.sh` downloads `premake-core` and includes
  it in the source tarball. `debian/rules` builds premake5 from source
  (`Bootstrap.sh`) and uses it to generate Makefiles. No external premake5
  package needed on the build farm.
- **React UIs are pre-built:** `build-ppa.sh` injects the local `dist/` folders
  into the source tarball. Launchpad has no npm/Node.js — the UIs ship as-is.
- Launchpad builds run in a **clean chroot** with no network access after
  source download, so all build dependencies must be declared in `debian/control`.
- `debian/rules` installs the shared `packaging/Linux/jarvisagent-launcher.sh`
  as `/usr/bin/jarvisagent` (same launcher used by RPM and Arch).
- `debian/postinst` only prints first-launch instructions (no system-wide venv).
  Python venv is created per-user by the launcher on first run.

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
| libpq (PostgreSQL) | `libpq-devel` |
| premake5 | Not in repos — download or build from source |
| Node.js + npm | `nodejs` `npm` |

| Runtime dep | RPM package |
|-------------|---------------|
| Python 3 | `python3` `python3-pip` |
| zlib | `zlib` |
| libpq (PostgreSQL) | `libpq` |
| bash | `bash` |

**Files:**
- `jarvisagent.spec` — RPM spec file (for `rpmbuild`)
- `build-rpm.sh` — build script (supports `rpmbuild` and `fpm` fallback, `--dry-run` option)
- `postinst.sh` / `postrm.sh` — post-install/remove hooks (for `fpm`)
- `prerequisites.sh` — installs all build + runtime dependencies (handles Rocky 9 vs 10+ Node.js differences)

**Build & install (Fedora/RHEL/Rocky):**
```bash
# From packaging/Linux/RPM/
sudo ./prerequisites.sh   # one-time: install deps
./build-rpm.sh
sudo dnf install build/jarvisagent-*_x86_64.rpm
jarvisagent               # creates ~/JarvisAgent, sets up venv, opens browser
```

**Uninstall:**
```bash
sudo dnf remove jarvisagent
rm -rf ~/JarvisAgent/   # remove user data
```

---

### AppImage

**Format:** Self-contained `.AppImage` bundle
**Directory:** `packaging/Linux/AppImage/`
**Status:** Build script + AppRun created

**Approach:**
- Bundle the release binary, React UI dist/ assets, and scripts into an AppDir.
- Use `linuxdeploy` (preferred, bundles shared libs) or `appimagetool` to generate the AppImage.
- On first launch, `AppRun` creates `~/JarvisAgent` with symlinks to read-only assets and real directories for writable data (`queue/`, `log/`, `workflows/`).
- Python venv is created on first run in the user's data directory.
- Override data dir with `JARVISAGENT_DATA=/path/to/dir`.

**Files:**
- `AppRun` — entry point script (symlinks assets, creates venv on first run)
- `jarvisagent.desktop` — FreeDesktop desktop entry
- `build-appimage.sh` — build script (`--dry-run` option, supports linuxdeploy and appimagetool)

**Build:**
```bash
# From packaging/Linux/AppImage/
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
rm -rf ~/JarvisAgent   # remove data directory
```

---

### Flatpak

**Format:** Flatpak bundle (`.flatpak`)
**Directory:** `packaging/Linux/Flatpak/`
**Status:** Manifest + wrapper + build script created

**Approach:**
- Flatpak manifest (`com.jctechnolabs.JarvisAgent.yml`) based on `org.freedesktop.Sdk//24.08`.
- Builds premake5 from source inside the Flatpak build, then builds JarvisAgent C++ + React UIs.
- Wrapper script creates `~/JarvisAgent` with symlinks to read-only assets and writable dirs.
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
./build-flatpak.sh
```

**Install & run:**
```bash
flatpak install --user build/JarvisAgent.flatpak
flatpak run com.jctechnolabs.JarvisAgent
```

**Uninstall:**
```bash
flatpak uninstall --user com.jctechnolabs.JarvisAgent
rm -rf ~/JarvisAgent   # remove user data
```

**Data directory:** `~/JarvisAgent` (config, queue, log, workflows)

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
- `jarvisagent.rb` — Homebrew formula (builds from source, user-space launcher)
- `build-dmg.sh` — .dmg build script (`--dry-run` option, creates .app bundle + Info.plist)

**Homebrew install:**
```bash
brew tap beaumanvienna/jarvisagent
brew install jarvisagent
jarvisagent                          # creates ~/JarvisAgent, sets up venv, opens browser
jarvisagent --home /path/to/dir      # custom working directory
jarvisagent --no-browser             # skip browser launch
```

**.dmg build & install:**
```bash
# From packaging/macOS/Tahoe/
./build-dmg.sh
# Open .dmg, drag JarvisAgent.app to /Applications
# Launch: open /Applications/JarvisAgent.app (or from Terminal)
```

**Uninstall:**
```bash
# Homebrew
brew uninstall jarvisagent

# .dmg
rm -rf /Applications/JarvisAgent.app

# User data (both Homebrew and .dmg)
rm -rf ~/JarvisAgent/
```

**Data directory:** `~/JarvisAgent/` (config, queue, log, workflows, .venv)

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
- `jarvisagent.wxs` — WiX v3 manifest for MSI installer
- `jarvisagent.bat` — user-space launcher (junctions for read-only assets, venv setup, browser launch)
- `setup-venv.bat` — standalone venv creation helper (for manual repair)

**Portable .zip build & install:**
```powershell
# From packaging\Windows\11\
.\build-zip.ps1
# Extract build\JarvisAgent-x64.zip
# Run jarvisagent.bat
```

**MSI build & install:**
```powershell
.\build-zip.ps1          # stage the package tree first
.\build-msi.ps1          # build the MSI (requires WiX v3 on PATH)
# Double-click build\JarvisAgent-<version>-x64.msi
# Run: jarvisagent
```

**After install (both MSI and portable):**
```
jarvisagent                             # creates %USERPROFILE%\JarvisAgent, venv, opens browser
jarvisagent --home C:\path\to\dir       # custom working directory
jarvisagent --no-browser                # skip browser launch
```

**MSI install location:**
- Read-only assets at `C:\Program Files\JarvisAgent\`
- The installer adds `C:\Program Files\JarvisAgent\` to the system PATH
- User working directory at `%USERPROFILE%\JarvisAgent\` (created on first run)
- Uses directory junctions (`mklink /J`, no admin required) for read-only assets

**Uninstall:**
```
# MSI: Go to the Windows Settings → Apps → JarvisAgent → Uninstall
#   or: msiexec /x JarvisAgent-<version>-x64.msi
# Portable: delete the extracted folder
# User data (both MSI and portable):
rmdir /s /q %USERPROFILE%\JarvisAgent
```

**Data directory:** `%USERPROFILE%\JarvisAgent\` (config, queue, log, workflows, .venv)

---

## Docker (all platforms)

**Format:** OCI container image on GHCR
**Status:** Implemented (`docker-publish.yml`)

The Docker image is already built and pushed to `ghcr.io` on every push to main/master/develop. This is the simplest cross-platform deployment — users with Docker can pull and run without any local build.

**Install & run:**
```bash
# Using the helper script (recommended)
./scripts/run-docker.sh              # Linux / macOS / Git Bash
./scripts/run-docker.sh /custom/path # custom data directory
```

```powershell
# Windows PowerShell
.\scripts\run-docker.ps1
.\scripts\run-docker.ps1 -DataDir C:\path   # custom data directory
```

Or manually:
```bash
docker pull ghcr.io/beaumanvienna/jarvisagent:latest
mkdir -p ~/JarvisAgent
docker run -it --rm \
  -p 8080:8080 \
  -v ~/JarvisAgent:/app \
  ghcr.io/beaumanvienna/jarvisagent:latest
```

- Dashboard: http://localhost:8080
- Workflow Editor: http://localhost:8080/editor
- The `-v` flag mounts `~/JarvisAgent` on the host so workflows, AI keys, and outputs persist across container restarts.

**MCP sidecar (optional):**

The `code/mcp/` directory contains a standalone MCP server that exposes j9t workflows to Claude Desktop and Claude Code. It can run as a Docker sidecar:

```yaml
# In docker-compose.example.yml
mcp:
  build: ./mcp
  depends_on: [jarvisagent]
  environment:
    J9T_URL: http://jarvisagent:8080
    # Provide the MCP API key via a mounted secret file, or inline as J9T_TOKEN=mcp_...
    J9T_TOKEN_FILE: /secrets/mcp_key
  volumes:
    - ./data:/app:ro
    - ./secrets:/secrets:ro
```

Note: `http://jarvisagent:8080` is the default. Check `config.json` for the actual port (`"port"` field) and whether TLS is enabled (`TlsCert`/`TlsKey`) — if TLS is configured, use `https://` and the corresponding port (default 8443).

See `code/mcp/README.md` for full details.

**Data directory:** `~/JarvisAgent` (mounted at `/app` inside the container)

**Remove:**
```bash
docker rmi ghcr.io/beaumanvienna/jarvisagent:latest
rm -rf ~/JarvisAgent   # remove user data
```

---

## CI Artifacts

All packages are built automatically by GitHub Actions on every push.
Download artifacts from the Actions tab on GitHub.

| Artifact | Format | CI Workflow / Job |
|----------|--------|-------------------|
| `JarvisAgent-<version>-deb` | `.deb` | linux-workflow / package-deb |
| `JarvisAgent-<version>-AppImage` | `.AppImage` | linux-workflow / package-appimage |
| `JarvisAgent-<version>-rpm` | `.rpm` | linux-workflow / package-rpm |
| `JarvisAgent-<version>-arch` | `.pkg.tar.zst` | linux-workflow / package-arch |
| `JarvisAgent-<version>-flatpak` | `.flatpak` | linux-workflow / package-flatpak |
| `JarvisAgent-<version>-macOS-dmg` | `.dmg` | macos-workflow / build-macos |
| `JarvisAgent-<version>-Windows-zip` | `.zip` | windows-workflow / build-windows |
| `JarvisAgent-<version>-Windows-msi` | `.msi` | windows-workflow / build-windows |

---

## Priority Order

1. **Arch** (Manjaro) — current dev machine, test immediately
2. **Ubuntu 24.04** — most common Linux desktop/server, CI already validates
3. **AppImage** — universal Linux, no root required
4. **RPM** — Fedora/RHEL/Rocky/CentOS ecosystem coverage
5. **Flatpak** — sandboxed distribution
6. **macOS Tahoe** — Homebrew tap + .dmg
7. **Windows 11** — MSI/MSIX installer
