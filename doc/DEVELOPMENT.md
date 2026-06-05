# Development Guide

This guide covers building JarvisAgent from source, the Python virtual environment, the React UIs, both editions (Studio / Engine), running, and Engine security.

To install pre-built packages instead, see [INSTALL.md](INSTALL.md).

---

## Clone

```bash
git clone https://github.com/beaumanvienna/JarvisAgent
cd JarvisAgent
```

---

## Dependencies

JarvisAgent depends on:

- **C++ toolchain** — C++23-capable gcc (≥12) or clang (≥16 with libc++, or ≥19 with libstdc++), plus `make`.  See **C++23 toolchain notes** below if you're on clang 18 or Rocky 9.
- **Python 3** and development headers (on Ubuntu/Debian: `python3`, `python3-dev`)
- **libz** (Linux — linked at build time; vendored on Windows; included in Xcode SDK on macOS)
- **libpq** — PostgreSQL client library, required by the DB-query cloud connector on all platforms
- **pkg-config** — used by premake5 to discover libpq
- **Node.js + npm** — to build the React UIs (dashboard + workflow editor)
- **premake5**
- **markitdown** (document conversion)
- **pandoc + pdflatex** (Markdown → PDF, required for PDF workflows; see platform-specific install commands below)
- **mmdc** — `@mermaid-js/mermaid-cli` (Mermaid diagram rendering — installed automatically by the launcher script)

> OpenSSL and libcurl are vendored in the repository and built from source on all platforms.

---

## Linux

Package names vary by distribution; the list you need is:

- C++ compiler and `make` (e.g. `build-essential` or `gcc-c++`)
- `pkg-config`
- Python 3 + development headers + venv + pip
- zlib development headers
- libpq development headers (PostgreSQL client)
- Node.js + npm
- premake5 (not packaged on most distros — build from source)

### Ubuntu / Debian (concrete example)

```bash
sudo apt install -y build-essential pkg-config \
                    python3 python3-pip python3-dev python3-venv \
                    zlib1g-dev libpq-dev nodejs npm
```

Premake5 — clone the `v5.0.0-beta8` tag, bootstrap, install:

```bash
git clone --depth 1 --branch v5.0.0-beta8 https://github.com/premake/premake-core
cd premake-core
make -f Bootstrap.mak linux
sudo cp bin/release/premake5 /usr/bin/
```

> **Why pinned:** `v5.0.0-beta8` is the first stable tag that accepts `cppdialect "C++23"`. Older tags (incl. `v5.0.0-beta2` and most Linux distro packages) error out with `invalid value 'C++23' for cppdialect`. CI is pinned to the same tag across Linux / macOS / Windows / Docker.

Other distros (Fedora / RHEL / Arch / …): install the equivalent packages from the list above, then build premake5 the same way from the same tag (Fedora needs `libuuid-devel` for the Bootstrap compile).

### C++23 toolchain notes

j9t builds at `cppdialect "C++23"` since Sitting 7a (2026-05-19). The matrix is fine on stock compilers everywhere EXCEPT two corner cases:

- **Clang ≤18 on Linux** — libstdc++'s `<expected>` header guards on `__cpp_concepts >= 202002L`, but clang 18 reports `201907L` (fixed in clang 19+).  Workaround: `sudo apt install libc++-18-dev libc++abi-18-dev` and build via `premake5 gmake --clang` — the `--clang` opt-in routes through libc++ (`-stdlib=libc++` in build + link flags) which ships its own `<expected>` without the guard.  Remove the `--clang` option once clang ≥19 lands in Ubuntu LTS.
- **Rocky Linux 9 RPM build** — system gcc is 11.5 (predates libstdc++'s `<expected>`, which arrived in libstdc++ 12).  The CI job installs `gcc-toolset-13-gcc gcc-toolset-13-gcc-c++` from the standard AppStream repo and wraps the build with `scl enable gcc-toolset-13 -- bash build-rpm.sh`.  The resulting RPM binary links to the system `libstdc++.so.6` at runtime (via embedded `libstdc++_nonshared.a` for the new ABI symbols), so it's runtime-portable to any stock Rocky 9 box without needing gcc-toolset installed.

Everything else (Ubuntu CI gcc 13, macOS Apple Clang 17 + libc++ 19, Windows MSVC 19.50, Arch gcc 16, Flatpak SDK 24.08 gcc 14, Docker linux/amd64+arm64 Ubuntu 24.04 gcc 13) ships `<expected>` natively at the current default compiler version.

---

## macOS

Install the build prerequisites via Homebrew:

```bash
brew install premake node python3 libpq pkg-config
```

`libpq` is keg-only on Homebrew (avoids clashing with a full PostgreSQL install), so you must expose it to `pkg-config` before running premake:

```bash
export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:$PKG_CONFIG_PATH"
```

Add that `export` line to your `~/.zshrc` (or `~/.bash_profile`) so it persists across sessions. A successful `premake5 gmake` will print `>>> libpq (macOS): -I/opt/homebrew/opt/libpq/include …` — if it prints "libpq: not found via pkg-config" instead, the env var is missing and the build will fail with `fatal error: 'libpq-fe.h' file not found`.

> Optional: `export MAKEFLAGS=-j$(sysctl -n hw.ncpu)` in your shell rc so every `make` invocation parallelizes across your cores.

---

## Windows

Install [Python 3](https://www.python.org/downloads/) (check **"Add to PATH"**). Premake5: download the Windows binary from [premake.github.io](https://premake.github.io/download) and add it to your PATH.

### Windows: PowerShell (default) or Bash

On Windows, JarvisAgent's shell-based workflow tasks run through **PowerShell** by default. No extra setup is required.

Shell scripts (`.sh`) have PowerShell siblings (`.ps1`) that are used automatically on Windows. New `.ps1` scripts generated by the AI assistant follow PowerShell conventions (`param()` block, `Set-StrictMode -Version Latest`, `$ErrorActionPreference = 'Stop'`).

If you prefer **MSYS2 / Git Bash**, set `"use_bash": true` in `config.json` (or toggle it in the Settings panel). JarvisAgent will probe `PATH` for `bash` at startup and fall back to PowerShell with a warning if bash is not found.

> **Recommendation:** use the default PowerShell mode unless you have existing `.sh` scripts without `.ps1` siblings or rely on POSIX tools not available in PowerShell.

---

## Python Virtual Environment

JarvisAgent's shell-based workflows call Python tools (`markitdown`) and system tools (`pandoc`, `mmdc`). The Python tools are installed in a **virtual environment** managed automatically by the launcher script. `mmdc` is installed automatically by the launcher on first run. Only `pandoc` and texlive require manual installation.

**Create and activate the venv** (one-time setup):

Linux / macOS:
```bash
python3 -m venv .venv   # use 'python3' here; after activation, 'python' works everywhere
source .venv/bin/activate
```

Windows (PowerShell):
```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
```

Windows (MSYS2 / Git Bash):
```bash
python -m venv .venv
source .venv/Scripts/activate
```

**Install Python dependencies** (inside the active venv, same on all platforms):

```bash
pip install "markitdown[all]"
```

**Optional — PDF workflow dependencies** (for `vehicleTroubleshootingGuide`-style flows):

```bash
# Linux (Ubuntu / Debian)
sudo apt install pandoc texlive-latex-base texlive-latex-extra texlive-fonts-recommended

# Linux (Fedora / RHEL)
sudo dnf install pandoc texlive-latex

# Linux (Arch)
sudo pacman -S pandoc texlive-bin

# macOS
brew install pandoc basictex

# Windows
choco install pandoc miktex
```

**Quick start** — use the launcher script (creates the venv automatically on first run):

```bash
./jarvisagent.sh
```

The script creates `.venv`, installs Python dependencies, and launches the release binary. On subsequent runs it activates the existing venv and starts JarvisAgent.

**Manual session** — if you prefer to manage the venv yourself:

Linux / macOS:
```bash
$ source .venv/bin/activate
(.venv) $ ./bin/Release/jarvisAgent-studio
```

Windows (MSYS2 / Git Bash):
```bash
$ source .venv/Scripts/activate
(.venv) $ ./bin/x64/Release/jarvisAgent-studio.exe
```

> **Note:** always activate the venv before running JarvisAgent so that `markitdown` is on PATH.

---

## React UIs (Dashboard & Workflow Editor)

The browser-based Dashboard and Workflow Editor are React apps built with [Vite](https://vite.dev/). You need **Node.js** (v18+) and **npm**.

**Install Node.js:**

| Platform | Command |
|---|---|
| Linux (Ubuntu / Debian) | `sudo apt install -y nodejs npm` (or use [nvm](https://github.com/nvm-sh/nvm)) |
| macOS | `brew install node` (or use [nvm](https://github.com/nvm-sh/nvm)) |
| Windows | Download the installer from [nodejs.org](https://nodejs.org/) |

**Build the UIs** (same on all platforms):

```bash
# Dashboard
cd code/frontend/dashboard/ui
npm install
npm run build

# Workflow Editor
cd ../../workflow-editor/ui
npm install
npm run build
```

The build output lands in `code/frontend/dashboard/ui/dist/` and `code/frontend/workflow-editor/ui/dist/` respectively. With TLS configured (`"TlsCert"`/`"TlsKey"` in `config.json` — the default in the repo config), JarvisAgent serves these at `https://localhost:8443` (dashboard) and `https://localhost:8443/editor`; without TLS it serves `http://localhost:8080`. Port configurable via `"port"`.

---

## MCP Sidecar (optional)

The `mcp/` directory contains a Node-based sidecar that exposes j9t workflows to Claude Desktop, Claude Code, and other MCP clients. Build it only if you plan to use MCP integration — it is not required for running j9t itself.

```bash
cd mcp
npm install
npm run build
```

The build output lands in `code/mcp/dist/`. See [code/mcp/README.md](code/mcp/README.md) for Claude Desktop / Claude Code integration snippets, environment variables (`J9T_URL`, `J9T_TOKEN`, `NODE_EXTRA_CA_CERTS` for self-signed TLS), and the full tool catalog (run plane, artifact plane, configure plane, observability).

---

## Editions

JarvisAgent builds as two editions from the same source tree. Each produces a distinctly named binary:

| Edition | Flag | Binary | Use case |
|---|---|---|---|
| **j9t Studio** (default) | *(none)* or `--studio` | `jarvisAgent-studio` | Developer workstation — workflow editor, AI assistant, config management |
| **j9t Engine** | `--engine` | `jarvisAgent-engine` | Production server — lean, no editing surface, full security stack (auth, RBAC, TLS, audit log) |

---

## Building

Generate build files with premake5, then build:

```bash
# Studio edition (default — full developer IDE)
premake5 gmake
make config=release          # → bin/Release/jarvisAgent-studio

# Engine edition (lean production server)
premake5 gmake --engine
make config=release          # → bin/Release/jarvisAgent-engine
```

On Windows, replace `gmake` with `vs2022` to generate a Visual Studio solution. On macOS, `xcode4` generates an Xcode project.

Each edition has its own intermediate directory (`bin-int/studio/` vs `bin-int/engine/`), so switching editions triggers a clean rebuild automatically.

Set parallel build flags for faster compilation:

```bash
export MAKEFLAGS=-j$(nproc)            # Linux
export MAKEFLAGS=-j$(sysctl -n hw.ncpu) # macOS
```

---

## Running

**Launcher script** (handles Python venv automatically):

```bash
./jarvisagent.sh              # Studio edition (default)
./jarvisagent.sh --engine     # Engine edition
```

**Direct binary** (requires an active Python venv):

```bash
./bin/Release/jarvisAgent-studio    # Studio
./bin/Release/jarvisAgent-engine    # Engine
```

- Dashboard: `https://localhost:8443` (the repo config ships TLS; `http://localhost:8080` without TLS)
- Workflow Editor: `https://localhost:8443/editor` (Studio only; `http://localhost:8080/editor` without TLS)
- Listen port configurable via `"port"` in `config.json` (default: 8443 HTTPS, or 8080 when TLS is off)

---

## Engine Security

Engine edition includes a full security stack. Studio has no browser-UI auth (developer workstation — localhost only) but uses the same MCP API key store for programmatic access.

**Authentication — exactly three paths, no legacy fallback:**

1. **MCP API key** — `Authorization: Bearer mcp_...`. Per-user credentials stored only as SHA-256 hashes in `mcp_keys.json.enc`. On first run with an empty store, j9t prints a bootstrap admin enrollment token to stderr.
2. **Dashboard session cookie** — set by `POST /api/auth/login` after the browser submits a valid MCP key. HttpOnly + SameSite=Strict (plus `Secure` under TLS), 8-hour sliding timeout.
3. **Gateway-trusted headers** — `X-Forwarded-User` / `X-Forwarded-Role` when `TrustedProxyHeader` / `TrustedRoleHeader` are configured.

Example: issue yourself the first admin key, then call the API:

```bash
# On first Engine start, copy the enroll_... token from j9t's stderr:
curl -sS -X POST http://host:8080/api/auth/mcp-keys/activate \
     -H 'Content-Type: application/json' \
     -d '{"enrollment_token":"enroll_..."}'
# Response contains "api_key":"mcp_...", shown exactly once — save it.

# Then authenticate subsequent requests:
curl -H "Authorization: Bearer mcp_..." http://host:8080/api/workflows
```

**RBAC:** three roles — `admin` (full access incl. shutdown, security logs, MCP key CRUD), `operator` (run control, app logs, adhoc submission when `adhoc_enabled`), `viewer` (read-only monitoring). The role travels on the key, session cookie, or gateway header.

**Key expiry + self-renewal:** MCP keys expire 90 days after activation. Users can self-renew within the window via `POST /api/auth/mcp-keys/self-renew` without admin involvement; old keys enter a 24-hour grace period. Expired keys require a fresh admin enrollment.

**Defense layers:** per-IP rate limiting (100 req/min, burst 20), failed auth lockout (10 failures → 15-min IP ban), request body size limit (configurable, default 10 MB), security response headers (CSP, X-Frame-Options, HSTS, Referrer-Policy).

**Audit logging:** all auth events, webhook decisions, adhoc submissions, and run control actions logged to `log/security.txt` with IP, user identity, role, and endpoint. Viewable in the dashboard's Security tab.

**TLS:** built-in HTTPS via `TlsCert`/`TlsKey` in `config.json` (port 8443), or deploy behind a TLS-terminating reverse proxy.

**Dashboard:** the login page accepts MCP keys only; successful login sets a session cookie that flows automatically on subsequent requests. A **Logout** button calls `POST /api/auth/logout` to destroy the server-side session.

**WebSocket:** browser upgrades are validated at the handshake via the session cookie (`.onaccept` hook); no in-band auth message.

**Public endpoints** (no auth): `GET /api/status`, `GET /` (dashboard HTML shell), `POST /api/auth/mcp-keys/activate` (enrollment token *is* the auth), `POST /api/auth/login` (MCP key *is* the auth).

**Webhook endpoints** use per-workflow HMAC-SHA256 signatures (separate from MCP auth). In Engine mode, a webhook secret is mandatory.

See [doc/cyber security.md](doc/cyber%20security.md) for the full threat model, deployment architecture, and operator responsibilities.

---

## Updating

```bash
git pull && git submodule update --init --recursive
```

Use `premake5 clean` to clean the project from build artifacts.
