# Installation Guide

This guide covers installing JarvisAgent from pre-built packages on all supported platforms. To build from source instead, see [DEVELOPMENT.md](DEVELOPMENT.md).

---

## Pre-built Packages

Pre-built packages for all platforms are available as GitHub Actions artifacts. Go to the [Actions](https://github.com/beaumanvienna/JarvisAgent/actions) tab and download from the latest successful run.

| Platform | Format | Artifact |
|---|---|---|
| Ubuntu / Debian | `.deb` | `JarvisAgent-<version>-deb` |
| Fedora / RHEL / Rocky | `.rpm` | `JarvisAgent-<version>-rpm` |
| Arch / Manjaro | `.pkg.tar.zst` | `JarvisAgent-<version>-arch` |
| Any Linux | `.AppImage` | `JarvisAgent-<version>-AppImage` |
| Any Linux (sandboxed) | `.flatpak` | `JarvisAgent-<version>-flatpak` |
| macOS | `.dmg` | `JarvisAgent-<version>-macOS-dmg` |
| Windows (portable) | `.zip` | `JarvisAgent-<version>-Windows-zip` |
| Windows (installer) | `.msi` | `JarvisAgent-<version>-Windows-msi` |
| Docker | OCI image | `ghcr.io/beaumanvienna/jarvisagent:latest` |

> **Note:** version numbers in the commands below are examples — replace with the version you downloaded.

All Linux packages install to `/opt/jarvisagent/` with a launcher at `/usr/bin/jarvisagent`. On first run the launcher creates a per-user working directory at `~/JarvisAgent` containing config, workflows, and a Python virtual environment. CLI options: `--home DIR` (custom path), `--no-browser` (skip opening dashboard).

---

## Ubuntu / Debian / Zorin / Linux Mint / Pop!_OS

Recommended — install from the PPA:

```bash
sudo add-apt-repository ppa:beauman/marley
sudo apt update
sudo apt install jarvisagent
jarvisagent
```

Or install a local `.deb`:

```bash
sudo dpkg -i jarvisagent_*_amd64.deb
sudo apt install -f
jarvisagent
```

Uninstall:

```bash
sudo apt remove jarvisagent
```

---

## Fedora / RHEL / Rocky / CentOS

```bash
sudo dnf install ./jarvisagent-0.75-1.x86_64.rpm
jarvisagent
```

Uninstall:

```bash
sudo dnf remove jarvisagent
```

---

## Arch / Manjaro

```bash
sudo pacman -U jarvisagent-0.75-1-x86_64.pkg.tar.zst
jarvisagent
```

Uninstall:

```bash
sudo pacman -R jarvisagent
```

---

## AppImage

```bash
chmod +x JarvisAgent-x86_64.AppImage
./JarvisAgent-x86_64.AppImage
```

On first run the wrapper creates `~/JarvisAgent` with example workflows, a default `config.json`, and a Python virtual environment.

---

## Flatpak

```bash
flatpak install --user JarvisAgent.flatpak
flatpak run com.jctechnolabs.JarvisAgent
```

On first run the wrapper creates `~/JarvisAgent` with example workflows, a default `config.json`, and a Python virtual environment.

Uninstall:

```bash
flatpak uninstall --user com.jctechnolabs.JarvisAgent
# and remove user data in ~/JarvisAgent
```

---

## macOS DMG

Open the `.dmg`, drag `JarvisAgent.app` to `/Applications`, then launch:

```bash
open /Applications/JarvisAgent.app
```

On first run the launcher creates `~/JarvisAgent` with config, workflows, and a Python venv.

Uninstall:

```bash
rm -rf /Applications/JarvisAgent.app
# and remove user data in ~/JarvisAgent
```

---

## Windows MSI

Double-click the `.msi` installer. After installation, run from any terminal:

```text
jarvisagent
```

Installs to `C:\Program Files\JarvisAgent\` and adds it to the system PATH. On first run the launcher creates `%USERPROFILE%\JarvisAgent` with config, workflows, and a Python venv.

Uninstall: Windows Settings → Apps → JarvisAgent → Uninstall.

---

## Windows ZIP (Portable)

Extract the `.zip` and run `jarvisagent.bat` from the extracted folder.

---

## Docker

Helper scripts:

```bash
./scripts/run-docker.sh                    # interactive with TUI
./scripts/run-docker.sh --headless         # headless (no TUI, web only)
./scripts/run-docker.sh /custom/path       # custom data directory
./scripts/run-docker.sh --headless /path   # headless + custom data dir
```

PowerShell:

```powershell
.\scripts\run-docker.ps1                    # Windows PowerShell
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

- Dashboard: `http://localhost:8080` (or `https://localhost:8443` with TLS)
- Workflow Editor: `http://localhost:8080/editor` (or `https://localhost:8443/editor` with TLS)
- The `-v` flag mounts `~/JarvisAgent` on the host so workflows, AI keys, and outputs persist across container restarts.

---

## First Run — AI Provider Setup

1. Open the Workflow Editor at `http://localhost:8080/editor` (or `https://localhost:8443/editor` with TLS).
2. Go to **AI Keys** → **+ Add Key** — enter a name (e.g. `openai`) and paste your API key, then click **Create**. Click **Save Encrypted** to persist the key.
3. Go to **AI Manager** — select your key from the **Key** dropdown for each provider interface, then click **Save to config.json**.

---

## Notes

- OpenSSL and libcurl are vendored in the repository and built from source on all platforms.
- User data persists in `~/JarvisAgent` (or `%USERPROFILE%\JarvisAgent` on Windows).
- See [packaging/packaging.md](packaging/packaging.md) for build scripts and packaging-internal details.
- To build from source instead of installing pre-built packages, see [DEVELOPMENT.md](DEVELOPMENT.md).
