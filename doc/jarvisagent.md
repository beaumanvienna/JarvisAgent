# jarvisagent(1) — JarvisAgent User Manual

**Version 0.9** — April 2026

**Contents:**
[Name](#name) ·
[Synopsis](#synopsis) ·
[Description](#description) ·
[Editions](#editions) ·
[Options](#options) ·
[Installation](#installation) ·
[Configuration](#configuration) ·
[Environment](#environment) ·
[AI Setup](#ai-setup) ·
[Workflows](#workflows) ·
[Workflow Editor](#workflow-editor) ·
[Dashboard](#dashboard) ·
[Security](#security) ·
[Files](#files) ·
[See Also](#see-also)

---

## NAME

**jarvisAgent** — parallel AI-driven automation engine with visual workflow editor

## SYNOPSIS

```
jarvisagent.sh [--studio | --engine] [--help]
jarvisAgent-studio [--help] [--version]
jarvisAgent-engine [--help] [--version]
```

## DESCRIPTION

JarvisAgent is a C++ backend / React frontend application for parallel AI-driven automation.
Its engine core dispatches many concurrent AI requests in parallel — think requirements analysis
across hundreds of items, stock-portfolio deep-research across every position, or chapter-by-chapter
processing of entire PDF books.

Office documents (Word, Excel, PowerPoint, PDF) are automatically converted to Markdown via
Microsoft MarkItDown — and chunked when too large — before being sent to the AI. JarvisAgent
is file-oriented by design: all inputs, outputs, and intermediate results live on disk, making it
a natural fit for engineering environments with large file landscapes.

Workflows let you chain serial and parallel tasks — AI calls, Python scripts, shell commands,
or native C++ — in a visual graph editor with various trigger types (manual, cron, file_watch).

The application ships in two editions — **Studio** (full developer IDE) and **Engine** (lean
production server) — each with its own binary and security profile. See [Editions](#editions)
below.

The application ships with an ncurses terminal UI for local or SSH sessions, a browser-based
React dashboard for remote monitoring, and a visual workflow editor. It compiles and runs on
Linux, macOS, and Windows.

## EDITIONS

JarvisAgent builds as two distinct binaries selected at build time (see [Installation](#installation)):

| | j9t Studio (default) | j9t Engine |
|-|----------------------|------------|
| **Binary** | `jarvisAgent-studio` | `jarvisAgent-engine` |
| **Purpose** | Developer workstation | Production server |
| **Network exposure** | Localhost only | LAN / internet |
| **Authentication** | None | Bearer token (admin), HMAC-SHA256 (webhooks) |
| **RBAC** | N/A | 3 roles: admin, operator, viewer |
| **Rate limiting** | None | Per-IP token bucket (100 req/min, burst 20) |
| **Audit logging** | None | `log/security.txt` (rotating, 10 MB x 5) |
| **Request body limit** | None | Configurable (default 10 MB) |
| **Workflow CRUD** | Full | Not available (compile-time removed) |
| **AI assistant** | Full | Not available (compile-time removed) |
| **AI JCWF generation** | Full | Not available (compile-time removed) |
| **Attack surface** | Full feature set | Minimal — runtime + monitoring only |

Studio is designed for single-developer use on a local machine. Engine is designed for
production deployments behind an API gateway. See [Security](#security) for details.

## OPTIONS

**Launcher flags** (for `jarvisagent.sh`):

- **`--studio`** — Launch the Studio edition (default).
- **`--engine`** — Launch the Engine edition.
- **`--help`** — Show usage information.

**Binary flags** (for `jarvisAgent-studio` / `jarvisAgent-engine`):

- **`--help`**, **`-h`** — Show a help message and exit.
- **`--version`**, **`-v`** — Print the version number and exit.

Unknown options cause an error message and a non-zero exit code.

## INSTALLATION

JarvisAgent is available as pre-built packages for all major platforms:

| Platform | Formats |
|----------|---------|
| **Linux** | DEB, RPM, Arch (PKGBUILD), Flatpak, AppImage |
| **macOS** | DMG, Homebrew formula |
| **Windows** | MSI installer, portable ZIP |
| **Docker** | `ghcr.io/beaumanvienna/jarvisagent` |

For building from source and detailed install/uninstall instructions for each package format,
see the project **README.md** and **packaging/packaging.md**.

### Shell completions

Shell completion scripts for the launcher are included in `integration/completions/`:

- **Bash:** `source /path/to/jarvisAgent/integration/completions/jarvisagent.bash` (or add to `~/.bashrc`)
- **Zsh:** Add the completions directory to `fpath` and run `compinit`:
  ```bash
  fpath=(/path/to/jarvisAgent/integration/completions $fpath)
  compinit
  ```

GitHub repository: https://github.com/beaumanvienna/JarvisAgent

## CONFIGURATION

JarvisAgent reads `config.json` from the current working directory at startup.
If the file is missing, the application prints an error and exits.

The following fields are recognized:

- **`"file format identifier"`** — (number) Internal format version for the config file.
- **`"description"`** — (string) A human-readable description of this configuration.
- **`"author"`** — (string) Author or owner of this configuration.
- **`"queue folder"`** — (string, **required**) Relative or absolute path to the queue directory. JarvisAgent monitors this folder for incoming files that trigger AI requests.
- **`"workflows folder"`** — (string) Relative or absolute path to the directory containing `.jcwf` workflow definition files. Defaults to `workflows/`.
- **`"port"`** — (number) Web server listen port. `0` = auto-select (8080 for HTTP, 8443 for HTTPS). Valid range: 1–65535. Default: `0`.
- **`"max threads"`** — (number) Worker-thread pool size. Default: 16. Valid range: 1–256.
- **`"max inflight ai calls"`** — (number) Maximum concurrent AI requests dispatched via HTTP/2. Decoupled from thread pool size since requests are multiplexed on a single I/O thread. Default: 100. Valid range: 1–1000.
- **`"python engines"`** — (number) Number of Python sub-interpreters (each with its own GIL) for parallel Python task execution. Requires Python 3.12+. Default: 4. Valid range: 1–16.
- **`"engine sleep time in run loop in ms"`** — (number) Main loop sleep duration in milliseconds. Controls CPU usage vs. responsiveness. Default: 10. Valid range: 1–256.
- **`"max file size in kB"`** — (number) Maximum input file size in kilobytes. Files larger than this are chunked before being sent to the AI. Default: 20. Valid range: 1–256.
- **`"jcwf batch size"`** — (number) Number of tasks per batch when the AI JCWF generator fans out to multiple parallel calls for large workflows. Workflows with fewer tasks than this threshold are generated in a single call. Default: 10.
- **`"verbose"`** — (boolean) Enable verbose logging output.
- **`"keys_file"`** — (string) Path to the encrypted API keys file. Default: `keys.json.enc`.
- **`"use_bash"`** — (boolean) Windows only: prefer bash (MSYS2/Git Bash) over PowerShell for shell tasks. Default: `false`. Ignored on Linux/macOS.
- **`"TlsCert"`** — (string) Path to a PEM-format TLS certificate file. Both `TlsCert` and `TlsKey` must be set to enable HTTPS. See [Security](#security).
- **`"TlsKey"`** — (string) Path to a PEM-format TLS private key file.
- **`"TrustedProxyHeader"`** — (string) HTTP header name for gateway-injected user identity (e.g. `X-Forwarded-User`). Engine edition only. See [Security](#security).
- **`"TrustedRoleHeader"`** — (string) HTTP header name for gateway-injected user role (e.g. `X-Forwarded-Role`). Engine edition only.
- **`"MaxRequestBodyMB"`** — (number) Maximum HTTP request body size in megabytes. Oversized requests are rejected with HTTP 413 before parsing. Engine edition only. Default: 10.
- **`"API index"`** — (number) Zero-based index of the default AI interface to use from the `"API interfaces"` array.
- **`"API interfaces"`** — (array) List of AI provider configurations. Each entry is an object with:
  - **`"url"`** — (string, **required**) The API endpoint URL (e.g. `https://api.openai.com/v1/chat/completions`).
  - **`"model"`** — (string) The model name (e.g. `gpt-4o`, `gemini-2.5-flash`).
  - **`"API"`** — (string) The reply parser type:
    - `API1` — OpenAI-compatible chat completions (OpenAI, Google Gemini via OpenAI-compat endpoint, Ollama, any `/v1/chat/completions` provider).
    - `API2` — OpenAI Responses API (GPT-5 and later models).
    - `API3` — Google Gemini native API (uses `x-goog-api-key` header and `/models/{model}:generateContent` URL scheme).
  - **`"name"`** — (string) Human-readable name for this interface. Auto-generated from URL domain + model if omitted.
  - **`"description"`** — (string) Optional description of this interface.
  - **`"key_name"`** — (string) Name of the API key provider to use from the encrypted keys file (e.g. "openai", "google", "anthropic").

## ENVIRONMENT

- **`JARVIS_MASTER_PASSWORD`** — Master password for decrypting the encrypted API keys file (`keys.json.enc`). When this variable is set, JarvisAgent decrypts the keys file automatically at startup without prompting. This is especially useful for Docker containers and CI environments. The master password is never stored on disk.

- **`OPENAI_API_KEY`** — Backward-compatible fallback. If no encrypted keys file is found and this variable is set, JarvisAgent creates a single "openai" provider entry using the key value. This is a convenience for simple single-provider setups.

**API key priority order:**

1. Encrypted keys file (`keys.json.enc`) decrypted with `JARVIS_MASTER_PASSWORD`.
2. Plaintext keys file (`keys.json`) — development fallback only.
3. `OPENAI_API_KEY` environment variable.

## AI SETUP

To use AI-powered workflows, you need at least one API key configured.

### Setting up API keys

1. Open the workflow editor at `http://localhost:8080/editor`.
2. Navigate to the **Settings** page (gear icon) and select **AI Keys**.
3. Add a provider name (e.g. "openai", "google", "anthropic") and paste the corresponding API key.
4. Click **Save Encrypted**. You will be prompted for a master password. This encrypts all keys into `keys.json.enc` in the working directory.

### Setting up AI models

1. In the workflow editor, navigate to the **AI Manager** page.
2. Configure one or more AI interfaces: set the API endpoint URL, model name, parser type, and select which key provider to use. Use `API1` for OpenAI and compatible providers, `API2` for OpenAI Responses API (GPT-5+), or `API3` for Google Gemini native.
3. Set the default interface index or override per-task in workflow definitions.

Supported providers include OpenAI (API1), Google Gemini (API1 via OpenAI-compat endpoint, or API3 for the native endpoint),
Anthropic (API1), Ollama and any provider offering an OpenAI-compatible chat completions API (API1).
The OpenAI Responses API (GPT-5+) uses API2.

## WORKFLOWS

JarvisAgent uses **JC Workflow** definition files (`.jcwf`) to describe automation pipelines as directed acyclic graphs (DAGs).

A workflow consists of:

- **Tasks** — individual units of work: AI calls, shell commands, Python scripts, or internal actions. Tasks can run in parallel when their dependencies allow it.
- **Edges** — dependency and dataflow connections between tasks. A task only runs after all its upstream dependencies have completed.
- **Triggers** — how a workflow is started: manually, on a cron schedule, or when a file appears in a watched directory.
- **Filters** — per-item expansion from CSV files, text line lists, or queries. A single task definition can fan out into many parallel instances.

### Task types

- **`ai_call`** — Send a prompt (with optional file attachments) to an AI provider and capture the response.
- **`shell`** — Run a shell command or script. Stdout and stderr are captured.
- **`python`** — Call a Python function with structured inputs/outputs.
- **`internal`** — Built-in actions (e.g. file operations).

For the full specification including JSON schema, dataflow mapping, template syntax, filter types, and per-item expansion, see: **doc/JC_Workflow_Specification.md**

## WORKFLOW EDITOR

The visual workflow editor is a React application served by JarvisAgent at:

```
http://localhost:8080/editor
```

Key features:

- **Visual DAG editor** — drag-and-drop nodes, draw dependency and dataflow edges between tasks.
- **Task inspector** — configure task type, parameters, file inputs/outputs, working directory, timeout, and per-task AI interface override.
- **Workflow CRUD** — open, save, validate, run, and clean workflows.
- **Live run monitoring** — task state badges update in real time via WebSocket (running, succeeded, failed, skipped).
- **Run controls** — start, stop, pause, and resume workflow runs.
- **Template browser** — start from pre-built workflow templates.
- **Validation** — the backend validates the workflow structure and reports issues with severity tiers directly on the canvas.
- **Filter builder** — configure CSV, text_lines, or query filters for per-item fan-out.
- **Log viewer** — virtual-scrolling log display with search and run analysis (see [Dashboard](#dashboard) below for details).

## DASHBOARD

The React dashboard is served at:

```
http://localhost:8080
```

It has two tabs: a **Dashboard** overview and a **Log Viewer**.

### Dashboard tab

- **Status bar** — live WebSocket connection indicator, active workflow runs, AI session count, completed/failed counters, and a quit button.
- **Workflows panel** — lists all registered workflows with controls to run or clean each workflow.
- **Session managers panel** — shows active AI request sessions with real-time status updates.

### Log Viewer tab

A full-screen, virtual-scrolling log display (up to 100,000 lines).

- **Live streaming** — new log lines arrive in real time via WebSocket.
- **Auto-scroll** — follows the latest output; toggles between **Follow** and **Paused** mode. Click the button or scroll up to pause, click again to resume.
- **Search** — press `/` or `Ctrl+F` to search. Navigate matches with `Enter` (next) / `Shift+Enter` (previous). Match count and position are shown in the toolbar.
- **Color-coded log levels** — lines containing `[error]` are displayed in **red**; lines containing `[warning]` or `[warn]` are displayed in **yellow**.
- **Run Analyzer** — press `1` or click the **Analyze** button. An overlay panel appears showing:
  - Workflow ID, run ID, state, start/end timestamps.
  - Clickable log-region links (start line — end line) to jump directly to the run's output.
  - **◀ / ▶** arrows to cycle through previous runs (newest first).
  - **Issues list** — all warnings and errors within the run, color-coded by severity (red for errors, yellow for warnings).
  - **▲ / ▼** arrows to step through issues one by one; clicking an issue jumps to its log line.

## SECURITY

### Studio edition

Studio has **no authentication**. It is designed for single-developer use on localhost. The operator is responsible for not exposing the port beyond localhost, reviewing AI-generated scripts before accepting them, and keeping API keys secure.

### Engine edition

Engine is designed for production deployments and includes comprehensive security:

- **Bearer token authentication.** All admin endpoints require an `Authorization: Bearer <token>` header. The token is a 256-bit cryptographically random hex string, auto-generated on first start and stored in `engine_api_token.txt` (file permissions `600`). Token comparison uses constant-time logic.
- **Token expiration and auto-rotation.** Tokens expire after 90 days and are auto-rotated. A startup warning is logged 7 days before expiry. Manual rotation: delete `engine_api_token.txt` and restart.
- **Failed auth lockout.** 10 failed attempts from the same IP within 5 minutes triggers a 15-minute lockout (HTTP 403 with `Retry-After: 900`).
- **RBAC (Role-Based Access Control).** Three roles with descending privilege:
  - **admin** — full access including shutdown, security logs, and all operations.
  - **operator** — run control, workflow monitoring, application logs.
  - **viewer** — read-only dashboard, workflow list, run status.

  In gateway mode, the role comes from the `TrustedRoleHeader` (default: `viewer` if missing). In bearer-token mode, the token grants `admin`.
- **Per-IP rate limiting.** Token bucket algorithm: 100 requests/minute per IP, burst of 20. Rate-limited requests receive HTTP 429 with `Retry-After`.
- **HMAC-SHA256 webhook authentication.** Webhook triggers require a per-workflow secret. Callers must include `X-Webhook-Signature: sha256=<hex>` computed over the raw request body. In Engine mode, webhooks without a configured secret are rejected.
- **WebSocket authentication.** Clients must send `{"type":"auth","token":"<token>"}` as the first message.
- **Built-in TLS (HTTPS).** Set `"TlsCert"` and `"TlsKey"` in `config.json` to serve HTTPS (default port 8443). If only one field is set or the files don't exist, j9t refuses to start. When TLS is enabled, HSTS headers are added.
- **Gateway-trusted identity headers.** When deployed behind an API gateway (Kong, AWS API Gateway, Traefik, nginx), configure `"TrustedProxyHeader"` and `"TrustedRoleHeader"` in `config.json`. The gateway handles authentication (OIDC, MFA, SSO) and j9t reads the authenticated user and role from the injected headers.
- **Security audit logging.** All auth events are logged to `log/security.txt` (rotating, 10 MB x 5 files). Logged events: auth success/failure, rate limit triggers, lockout triggers, webhook accept/reject, shutdown requests, and run control actions — all with IP, user identity, and timestamps. Accessible via `GET /api/log/security` (admin role required) and the dashboard's Security tab.
- **Security response headers.** All responses include `Content-Security-Policy`, `X-Frame-Options: DENY`, `X-Content-Type-Options: nosniff`, `Referrer-Policy`, and `Permissions-Policy`.
- **Request body size limit.** Configurable via `"MaxRequestBodyMB"` (default 10 MB). Oversized requests are rejected with HTTP 413 before parsing.

For the complete security model, threat analysis, and operator responsibilities, see **doc/cyber security.md**.

## FILES

- **`config.json`** — Main configuration file (must exist in the working directory).
- **`keys.json.enc`** — Encrypted API keys (AES-256, master-password protected).
- **`keys.json`** — Plaintext API keys (development fallback, gitignored).
- **`engine_api_token.txt`** — Auto-generated admin bearer token (Engine edition only, file permissions `600`). Contains the token on line 1 and `issued_at=<ISO-8601>` on line 2.
- **`workflows/`** — Directory containing `.jcwf` workflow definition files.
- **`queue/`** — Monitored directory for incoming files.
- **`log/log.txt`** — Application log file.
- **`log/security.txt`** — Security audit log (Engine edition only, rotating 10 MB x 5 files).
- **`integration/completions/`** — Shell completion scripts for bash and zsh.
- **`.venv/`** — Python virtual environment (auto-created by launcher scripts). Contains markitdown. PDF workflows additionally require `pandoc` (system package) and `mmdc` (`npm install -g @mermaid-js/mermaid-cli@10.x`).

## SEE ALSO

- **Project README** — [README.md](https://github.com/beaumanvienna/JarvisAgent/blob/main/README.md)
- **Packaging and installation** — [packaging/packaging.md](https://github.com/beaumanvienna/JarvisAgent/blob/main/packaging/packaging.md)
- **JC Workflow Specification** — [doc/JC_Workflow_Specification.md](https://github.com/beaumanvienna/JarvisAgent/blob/main/doc/JC_Workflow_Specification.md)
- **REST API reference** — [doc/api-endpoints.md](https://github.com/beaumanvienna/JarvisAgent/blob/main/doc/api-endpoints.md)
- **Cyber security model** — [doc/cyber security.md](https://github.com/beaumanvienna/JarvisAgent/blob/main/doc/cyber%20security.md)
- **Key management internals** — [engine/keys.md](https://github.com/beaumanvienna/JarvisAgent/blob/main/engine/keys.md)

---

*JarvisAgent is developed by JC Technolabs — [GitHub](https://github.com/beaumanvienna/JarvisAgent)*
