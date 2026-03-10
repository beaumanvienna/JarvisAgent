![🐧Linux](https://github.com/beaumanvienna/JarvisAgent/actions/workflows/linux-workflow.yml/badge.svg)
![🪟Windows](https://github.com/beaumanvienna/JarvisAgent/actions/workflows/windows-workflow.yml/badge.svg)
![🍎macOS](https://github.com/beaumanvienna/JarvisAgent/actions/workflows/macos-workflow.yml/badge.svg)
![🐳Docker](https://github.com/beaumanvienna/JarvisAgent/actions/workflows/docker-publish.yml/badge.svg)
<br>
![DEB](https://img.shields.io/badge/📦_DEB-Ubuntu-E95420)
![RPM](https://img.shields.io/badge/📦_RPM-Fedora-51A2DA)
![Arch](https://img.shields.io/badge/📦_PKGBUILD-Arch-1793D1)
![AppImage](https://img.shields.io/badge/📦_AppImage-portable-333)
![Flatpak](https://img.shields.io/badge/📦_Flatpak-sandboxed-4A86CF)
![MSI](https://img.shields.io/badge/📦_MSI-Windows-0078D4)
![DMG](https://img.shields.io/badge/📦_DMG-macOS-999)

# JarvisAgent (j9t)

<br>

JarvisAgent is a **C++ backend / React frontend** application for parallel AI-driven automation.  <br>
<br>
Its engine core dispatches many concurrent AI requests — think parallel requirements analysis, stock-portfolio deep-research across every position, or chapter-by-chapter processing of entire PDF books.  <br>
<br>
Office documents (Word, Excel, PowerPoint, PDF) are automatically converted to Markdown via Microsoft's [MarkItDown](https://github.com/microsoft/markitdown) — and chunked when too large — before being sent to the AI. JarvisAgent is file-oriented by design: all inputs, outputs, and intermediate results live on disk, making it a natural fit for engineering environments with large file landscapes.  <br>
<br>
Workflows let you chain **serial and parallel tasks** — AI calls, Python scripts, shell commands, or native C++ — in a visual graph editor with various trigger types.  <br>
<br>
The application ships with an **ncurses terminal UI** for local or SSH sessions and a **browser-based React dashboard** for remote monitoring. It compiles under **Linux, macOS, and Windows**.  <br>
<br>

| Terminal UI | Dashboard | Workflow Editor |
|:-----------:|:---------:|:---------------:|
| ![Terminal UI](example/screenshot.png) | ![Dashboard](example/screenshot_dashboard.png) | ![Workflow Editor](example/screenshot_workflow_editor.png) |

<br>

---

## Layer Overview

| Layer | Responsibility | Status |
|-------|----------------|--------|
| **Engine** | Networking (`libcurl` + `openssl`), logging (`spdlog`), JSON parsing (`simdjson`), threading (`BS thread-pool`), profiling (`tracy`) | ✅ |
| **Event System** | Thread-safe atomic event queue and dispatcher for cross-thread communication | ✅ |
| **Workflow Runtime** | DAG-based workflow engine — serial/parallel task execution, cron & file-watch triggers, timezone-aware scheduling | ✅ |
| **Application** | Orchestrates queue handling, event dispatching, file tracking, and AI query flow | ✅ |
| **Config** | `config.json` with folder paths, thread count, AI backend model, and other settings | ✅ |
| **I/O** | File watcher, categorizer, environment assembly (STNG/CNTX/TASK), automatic binary detection and MarkItDown-based document conversion | ✅ |
| **Networking** | Asynchronous AI query dispatch (HTTP REST via libcurl) with multi-model selection | ✅ |
| **Frontend** | React workflow editor (ReactFlow graph UI), workflow list, live run monitoring via WebSocket | ✅ |

---

## File Categories

JarvisAgent processes different types of prompt files located in the queue folder defined in the configuration file `config.json`.  
Each file category serves a specific purpose, and files are identified using 4-letter, all-caps prefixes.

| Category | Description | Prefix | Example Filename |
|-----------|-------------|---------|------------------|
| **Settings** | Style, behavior, or tone modifiers (e.g., "write succinct", "use formal tone") | `STNG` | `STNG_write_succinct.txt` |
| **Context / Description** | Provides contextual or background information for AI prompts | `CNTX` | `CNTX_project_overview.txt` |
| **Task** | Defines the main task or instruction for the AI | `TASK` | `TASK_compare_requirements.txt` |
| **Provider** | AI provider configuration (endpoint, model) — content is **never** sent to the AI | `PROV` | `PROV_provider.json` |
| **Subfolders** | Contain additional prompt or requirement files, processed recursively | *(folder name itself)* | `queue/subproject/` |
| **Requirements** | Requirement files such as customer or system requirements | *(no prefix/PROB_)* | `REQ_vehicle_speed.txt` or `customer_requirement_001.txt` |

🧠 Categories **STNG**, **CNTX**, and **TASK** are combined into an **environment** used alongside each individual requirement file during processing.

---

## Architecture & Design Overview

- **JC Workflow Files (`.jcwf`)** — JSON-based workflow definitions that describe a DAG of tasks, their dependencies, triggers, filters, and data flow. Each task can be an `ai_call`, `shell` command, `python` script, or `internal` C++ module. Tasks with no mutual dependency run in parallel. The [JC Workflow Specification](doc/JC_Workflow_Specification.md) defines the format, path-resolution rules, and execution model.
- **Per-item fan-out and filters** — A filter (CSV, text_lines, or Polarion query) produces a list of items. A `per_item` task spawns one AI call per item, all running in parallel. For example, a 60-position stock portfolio CSV yields 60 concurrent dividend-lookup queries, each receiving only its own row as context. This isolation improves accuracy: the AI focuses on a single item rather than parsing a large table, and every response is independently verifiable. A downstream aggregation task then consumes all 60 results via a glob pattern.
- **Queue binding and environment assembly** — `ai_call` tasks declare inline STNG, TASK, CNTX, and PROB files. Template variables (`{{binding.field}}`) are expanded per filter item. The assembled files are written to disk and picked up by the session manager for dispatch.
- **REST API and WebSocket** — JarvisAgent exposes a full REST API (`/api/workflows`, `/api/workflows/<id>/run`, `/api/workflows/<id>/clean`, `/api/status`, etc.) for workflow CRUD, run control, and live status. A WebSocket channel pushes real-time task-state updates to the React dashboard and workflow editor. See [doc/api-endpoints.md](doc/api-endpoints.md).
- **Environment Files** — Files in categories STNG (Settings), CNTX (Context/Description), and TASK (Tasks). These form the shared environment or knowledge base.  
- **Query Files aka Requirement Files aka PROB Files** — Each represents a smaller task or requirement that is processed using the shared environment.  
- **File Watcher** — Monitors additions, modifications, and removals in the queue folder (including environment and query files).  
- **File Categorizer & Tracker** — Tracks which files belong to which category, monitors modification status, and provides content retrieval.  
- **Binary Detection & Conversion** — Detects binary document formats (PDF, DOCX, HTML, etc.) and uses MarkItDown to convert them to Markdown before querying the AI.  
- **CurlWrapper / REST Interface** — Handles communication with the AI provider's API (e.g., GPT-4 and GPT-5 models) via HTTP.  
- **Thread Pool / Parallel Processing** — Configured by `maxThreads` in `config.json`; handles multiple query tasks in parallel.  
- **JarvisAgent Application** — Orchestrates startup, event handling, file watching, categorization, and query dispatching.  
- **Core Engine** — Provides globally shared components (thread pool, event queue, logger, config, etc.).  
- **Terminal Renderer** — Uses PDcurses for advanced log and status display in the console.  

---

## State Machine & Processing Flow

JarvisAgent operates as a **reactive state machine** that responds to file changes:

1. **CompilingEnvironment** — Waits until all STNG, CNTX, and TASK files are available and up to date.  
2. **SendingQueries** — Dispatches requirement (REQ) files in parallel using the assembled environment.  
3. **AllQueriesSent** — Awaits completion of all query futures.  
4. **AllResponsesReceived** — Returns to idle until environment or requirements change.

Any detected file modification automatically triggers selective reprocessing:
- Environment changes ⇒ full environment rebuild.  
- Requirement changes ⇒ re-query for that specific file only.  
- Outputs are **regenerated only when inputs or the environment have changed**, preventing unnecessary re-queries.

---

## Design Highlights

- **Massively parallel AI engine** — Thread pool dispatches hundreds of concurrent AI requests for bulk processing workloads.  
- **Multi-model support** — Compatible with **GPT-4**, **GPT-4.1-mini**, and **GPT-5** through configurable API endpoints.  
- **Visual workflow editor** — React-based graph UI for building DAG workflows with drag-and-drop nodes, auto-layout, and live run status.  
- **Flexible task types** — Workflow tasks can be AI calls, Python scripts, shell commands, or native C++ — mixed freely in serial and parallel.  
- **Cron & trigger scheduling** — Cron triggers with IANA timezone support, file-watch triggers, manual triggers, and auto-start triggers.  
- **Office document conversion** — PDF, DOCX, XLSX, PPTX, and HTML are converted to Markdown via Microsoft MarkItDown before AI processing.  
- **Smart dependency tracking** — Re-evaluates files only when inputs or environment have changed, preventing unnecessary re-queries.  
- **Dual UI** — ncurses terminal UI for headless/SSH operation, plus a browser-based React dashboard for remote monitoring.  
- **Event-driven architecture** — Loosely coupled, non-blocking design with thread-safe event queue.  
- **Cross-platform** — Compiles and runs on **Linux** (GCC), **macOS** (Clang), and **Windows** (MSVC).  

---


## Example Queue Folder Layout

```text
queue/
├── STNG_be_succinct.txt
├── CNTX_project_overview.txt
├── TASK_compare_requirements.txt
├── REQ_vehicle_speed.txt
└── subproject/
    ├── STNG_be_formal.txt
    ├── CNTX_subtask.txt
    ├── TASK_subtask.txt
    └── REQ_subsystem_behavior.txt
```




---

## Example Workflows

| Workflow | Highlights |
|----------|------------|
| [Go-Kart Compliance Check](example/workflows/goKartComplianceCheck.md) | Polarion integration, per-item fan-out, `{{template}}` substitution |
| [Portfolio Dividend Analysis](example/workflows/portfolioDividendAnalysis.md) | CSV filter, 60-position fan-out, glob-based aggregation |
| [AI Car Maintenance Pipeline](example/workflows/aiCarMaintenancePipeline.md) | Retrieves user manuals based on AI categorization, multi-stage pipeline |
| [Vehicle Troubleshooting Guide](example/workflows/vehicleTroubleshootingGuide.md) | AI-generated flow charts based on user prompt, shell + AI pipeline |
| [Make-Style Build Example](example/workflows/make_example.md) | Classic dependency graph, file I/O, shell tasks |

---

## Planned Features

- [x] Docker deployment & CI/CD — 3-stage build, pushed to GHCR ([AhmetErenLacinbala](https://github.com/AhmetErenLacinbala))  
- [ ] Native Google Gemini reply parser (currently using OpenAI-compatible legacy endpoint)  
- [ ] n8n workflow integration ([AhmetErenLacinbala](https://github.com/AhmetErenLacinbala))  
- [ ] Enable HTTP/2 for improved network performance  

---

## Contributions

You are welcome to contribute!  
Please enable **clang-format** in your IDE. The coding style is **Allman**, and member fields of structs and classes use the `m_` + PascalCase convention.

---

## Pre-built Packages

Pre-built packages for all platforms are available as GitHub Actions artifacts.
Go to the [Actions](https://github.com/beaumanvienna/JarvisAgent/actions) tab and download from the latest successful run:

| Platform | Format | Artifact |
|----------|--------|----------|
| Ubuntu / Debian | `.deb` | `JarvisAgent-deb` |
| Fedora / RHEL / Rocky | `.rpm` | `JarvisAgent-rpm` |
| Arch / Manjaro | `.pkg.tar.zst` | `JarvisAgent-arch` |
| Any Linux | `.AppImage` | `JarvisAgent-AppImage` |
| Any Linux (sandboxed) | `.flatpak` | `JarvisAgent-flatpak` |
| macOS | `.dmg` | `JarvisAgent-macOS-dmg` |
| Windows (portable) | `.zip` | `JarvisAgent-Windows-zip` |
| Windows (installer) | `.msi` | `JarvisAgent-Windows-msi` |
| Docker | OCI image | `ghcr.io/beaumanvienna/jarvisagent:latest` |

**Linux packages** install to `/opt/jarvisagent/` with a launcher at `/usr/bin/jarvisagent`.
On first run, the launcher creates a per-user working directory at `~/JarvisAgent` with config,
workflows, and a Python virtual environment. Options: `--home DIR` (custom path),
`--no-browser` (skip opening dashboard).

**Flatpak:**

```bash
flatpak install --user JarvisAgent.flatpak
flatpak run com.jctechnolabs.JarvisAgent
```

On first run, the wrapper creates a working directory at `~/JarvisAgent`
with example workflows, a default `config.json`, and a Python virtual environment.

To uninstall:
```bash
flatpak uninstall --user com.jctechnolabs.JarvisAgent
# and remove user data in ~/JarvisAgent   
```

**Windows MSI** installs to `C:\Program Files\JarvisAgent\` and adds it to the system PATH.

**Docker:**

```bash
./scripts/run-docker.sh              # Linux / macOS / Git Bash
./scripts/run-docker.sh /custom/path # custom data directory
```

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

- Dashboard: http://localhost:8080
- Workflow Editor: http://localhost:8080/editor
- The `-v` flag mounts `~/JarvisAgent` on the host so workflows, AI keys, and outputs persist across container restarts.

**Setting up AI providers** (first run):

1. Open the Workflow Editor at http://localhost:8080/editor
2. Go to **AI Keys** → **+ Add Key** — enter a name (e.g. `openai`) and paste your API key, then click **Create**. Click **Save Encrypted** to persist the key.
3. Go to **AI Manager** — select your key from the **Key** dropdown for each provider interface, then click **Save to config.json**.

See [packaging/packaging.md](packaging/packaging.md) for build scripts and detailed instructions.

---

## Building from Source

JarvisAgent depends on
* Python 3 and development headers (on Ubuntu/Debian the packages are called `python3`, `python3-dev`)
* libz (Linux — linked at build time; vendored on Windows, included in Xcode SDK on macOS)
* premake5
* markitdown (document conversion)
* md2pdf-mermaid (Markdown → PDF with Mermaid diagram support)
* playwright (headless Chrome, used by md2pdf-mermaid)

> OpenSSL and libcurl are vendored in the repository and built from source on all platforms.

### Linux (Ubuntu / Debian)

```bash
sudo apt install -y python3 python3-pip python3-dev python3-venv zlib1g-dev
```

Premake5: `git clone https://github.com/premake/premake-core`, build it with `./Bootstrap.sh`, copy the executable to `/usr/bin`.

### Linux (Fedora / RHEL / Rocky Linux)

```bash
sudo dnf install -y gcc-c++ make python3 python3-devel python3-pip zlib-devel nodejs npm
```

On RHEL / Rocky Linux you may need EPEL for additional packages:
```bash
sudo dnf install -y epel-release
sudo dnf makecache
```

Premake5 is not in the Fedora/EPEL repos — build from source:
```bash
git clone https://github.com/premake/premake-core
cd premake-core
sudo dnf install -y libuuid-devel   # required by Bootstrap.sh
./Bootstrap.sh
sudo cp bin/release/premake5 /usr/bin/
```

### macOS

```bash
brew install python3
```

Premake5: download from [premake.github.io](https://premake.github.io/download) or build from source as above.

### Windows

Install [Python 3](https://www.python.org/downloads/) (make sure to check **"Add to PATH"**).  
Premake5: download the Windows binary from [premake.github.io](https://premake.github.io/download) and add it to your PATH.

---

### Windows: Use MSYS2 or Git Bash

On Windows, JarvisAgent's shell-based workflow tasks execute through `bash`. You must start JarvisAgent from an **MSYS2 terminal** or **Git Bash** — not from PowerShell or cmd.exe.

MSYS2 and Git Bash provide the POSIX shell environment (`bash`, `wc`, `dirname`, etc.) that the workflow scripts require. If you launch JarvisAgent from PowerShell or cmd.exe, all shell tasks will fail because `bash` is not on the PATH.

> **Recommendation:** Use MSYS2 if you need to install additional tools via `pacman`. Use Git Bash if you already have [Git for Windows](https://gitforwindows.org/) installed and prefer a lighter setup.

---

### Python Virtual Environment

JarvisAgent's shell-based workflows call Python tools (`markitdown`, `md2pdf`). These should be installed in a **virtual environment** and activated before starting JarvisAgent.

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

**Install the Python dependencies** (inside the active venv, same on all platforms):

```bash
pip install "markitdown[all]" md2pdf-mermaid playwright
playwright install chromium
```

**Quick start** — use the launcher script (creates the venv automatically on first run):

Linux / macOS:
```bash
./jarvisagent.sh
```

The script creates `.venv`, installs all Python dependencies, and launches the release binary.  
On subsequent runs it simply activates the existing venv and starts JarvisAgent.

**Manual session** — if you prefer to manage the venv yourself:

Linux / macOS:
```bash
$ source .venv/bin/activate
(.venv) $ ./bin/Release/jarvisAgent
```

Windows (MSYS2 / Git Bash):
```bash
$ source .venv/Scripts/activate
(.venv) $ ./bin/x64/Release/jarvisAgent.exe
```

> **Note:** Always activate the venv before running JarvisAgent so that `markitdown` and `md2pdf` are on the PATH.

---

### React UIs (Dashboard & Workflow Editor)

The browser-based Dashboard and Workflow Editor are React apps built with [Vite](https://vite.dev/).  
You need **Node.js** (v18+) and **npm** to build them.

**Install Node.js:**

| Platform | Command |
|----------|---------|
| **Linux (Ubuntu/Debian)** | `sudo apt install -y nodejs npm` (or use [nvm](https://github.com/nvm-sh/nvm)) |
| **macOS** | `brew install node` (or use [nvm](https://github.com/nvm-sh/nvm)) |
| **Windows** | Download the installer from [nodejs.org](https://nodejs.org/) |

**Build the UIs** (same on all platforms):

```bash
# Dashboard
cd dashboard/ui
npm install
npm run build

# Workflow Editor
cd ../../workflow-editor/ui
npm install
npm run build
```

The build output lands in `dashboard/ui/dist/` and `workflow-editor/ui/dist/` respectively.  
JarvisAgent serves these automatically at `http://localhost:8080` (dashboard) and `http://localhost:8080/editor` (workflow editor).

---

To clone the project, use:

```bash
git clone --recurse-submodules https://github.com/beaumanvienna/JarvisAgent
```
<br>
JarvisAgent is cross-platform. The project is defined in a Lua file for permake5.<br>
<br>
Run 

```bash
premake5 gmake 
```
to get a Makefile.<br><br>
Run

```bash
premake5 vs2022
```
to get a VS solution.<br><br>
Run 

```bash
premake5 xcode4
```
for Xcode on MacOS.<br>
<br>
<br>
If you created a Makefile, build the project with<br>
`export MAKEFLAGS=-j8` or however many CPU cores you want to use<br>
`make config=release verbose=1 && make config=debug verbose=1`<br>
<br>
Run with the launcher script (handles venv automatically):<br>
`./jarvisagent.sh`<br>
<br>
Or run the executable directly (requires an active venv):<br>
`./bin/Release/jarvisAgent` or `./bin/Debug/jarvisAgent`<br>
<br>
To update the source code, use<br>
`git pull && git submodule update --init --recursive`<br>
<br>
Use `premake5 clean` to clean the project from build artifacts.<br>
<br>
<br>
<br>

GPL-3.0  License © 2026 JC Technolabs
