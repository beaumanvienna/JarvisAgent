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

### Docker

The published image `ghcr.io/beaumanvienna/jarvisagent:latest` ships both editions
(Studio is launched by default). The `scripts/run-docker.sh` wrapper handles docker
group membership, data-directory persistence, and the HTTP/TLS mode switch.

**Default: plain HTTP on port 8080.** Out of the box the container serves the
dashboard and editor over plain HTTP:

```bash
./scripts/run-docker.sh              # interactive with TUI
./scripts/run-docker.sh --headless   # headless (web only)
```

Open `http://localhost:8080` (dashboard) and `http://localhost:8080/editor`
(workflow editor). Workflows, logs, keys, and `config.json` persist in
`~/JarvisAgent` — pass a trailing path to use a different directory.

On Windows, use `scripts\run-docker.ps1` with the same semantics.

**TLS mode: HTTPS on port 8443.** For HTTPS, generate a certificate (or supply one
from your PKI) and place the files under `<data_dir>/certs/`:

```bash
mkdir -p ~/JarvisAgent/certs
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout ~/JarvisAgent/certs/j9t-key.pem \
  -out    ~/JarvisAgent/certs/j9t-cert.pem \
  -days 365 \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
chmod 600 ~/JarvisAgent/certs/j9t-key.pem
```

Then launch the container in TLS mode:

```bash
./scripts/run-docker.sh --tls              # interactive + TUI, HTTPS on 8443
./scripts/run-docker.sh --tls --headless   # headless, HTTPS on 8443
```

Open `https://localhost:8443`. Self-signed certificates produce a browser warning —
accept it to proceed. On Windows, use `scripts\run-docker.ps1 -Tls`.

**Switching modes after first run.** On the very first start, the container seeds
`/app/config.json` from the image defaults. Subsequent starts reuse the existing
file. If you later want to switch between HTTP and TLS, do one of the following:

- Edit `~/JarvisAgent/config.json` directly: for TLS, set `"port": 8443` and add
  `"TlsCert": "certs/j9t-cert.pem"` + `"TlsKey": "certs/j9t-key.pem"`; for HTTP,
  remove those three fields.
- Delete `~/JarvisAgent/config.json` so the entrypoint re-seeds it on next start.

The entrypoint refuses to launch when the `--tls` flag disagrees with the config
file on disk and prints instructions matching your case.

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

- **`OPENAI_API_KEY`** — Bootstrapping fallback. If no encrypted keys file is found and this variable is set, JarvisAgent creates a single `openai` provider entry using the key value. This is a convenience for simple single-provider setups; encrypt keys as soon as practical.

The master password for `keys.json.enc` and `mcp_keys.json.enc` is supplied **only** at runtime via the dashboard login flow or `POST /api/settings/keys/unlock`. There is no environment-variable shortcut — the password is held exclusively in `mlock()`-protected memory (`SecureString`) and never lands in process listings, `docker inspect`, or crash dumps.

**API key priority order:**

1. Encrypted keys file (`keys.json.enc`) — unlocked at runtime via dashboard login or `POST /api/settings/keys/unlock`.
2. Plaintext keys file (`keys.json`) — development fallback only.
3. `OPENAI_API_KEY` environment variable.

## FIRST STEPS AFTER RESTART

Every time JarvisAgent starts (cold boot, `systemctl restart`, container restart), the encrypted key stores (`keys.json.enc` and `mcp_keys.json.enc`) are sealed — the master password is held only in `mlock()`-protected memory and is never persisted. An admin must unlock them before any credential-dependent work can proceed.

**What does not work until you unlock:**

- MCP API key authentication — all `Authorization: Bearer mcp_...` requests return 401 / 403 because the key hashes can't be compared.
- AI provider API calls — workflows with `ai_call` tasks fail at dispatch time with a clear "no providers configured" error.
- OAuth token refresh (OneDrive, Google Sheets, etc.) — access tokens won't be re-issued when they expire.
- Cloud connections that depend on encrypted credentials (Snowflake key-pair, Postgres/SMTP BasicAuth, Slack bot token, etc.).

**What does work:**

- `GET /api/status` — reports `"keys_unlocked": false` so monitors can alert.
- `POST /api/auth/mcp-keys/activate` — new users can exchange an enrollment token without the store being unlocked (the token itself is the auth).
- `POST /api/settings/keys/unlock` — the endpoint that unseals everything.
- Studio browser UI on localhost (no auth required).

**How to unlock:**

1. **Dashboard:** load the Engine dashboard in a browser, enter your MCP key on the login page. If the key store is sealed, the Settings modal will prompt for the master password; provide it once and the session proceeds normally afterwards.
2. **REST:** `curl -sS -X POST https://host:8443/api/settings/keys/unlock -H 'Content-Type: application/json' -d '{"master_password":"..."}'` — returns 200 with `"mcp_keys_loaded": true` on success, 401 on wrong password.
3. **CI / Docker / automation:** call the unlock endpoint from a startup script using a secret you mount in (e.g. from Docker secrets, AWS Secrets Manager, HashiCorp Vault). **Do not** try to use the old `JARVIS_MASTER_PASSWORD` environment variable — it was removed on purpose so the password never shows up in process listings or `docker inspect`.

**First-run only:** on the very first start with an empty MCP key store, JarvisAgent prints a bootstrap admin enrollment token to the application log (`log/log.txt` plus the TUI). Activate it via `POST /api/auth/mcp-keys/activate` to obtain the initial admin MCP key, then unlock as above on subsequent restarts.

## MCP API KEYS — ENROLLMENT, ACTIVATION, USE

Every credential in JarvisAgent is an **MCP API key** — a per-user, individually-revocable bearer token. Keys are prefixed `mcp_` because the same credential is used by the **M**odel **C**ontext **P**rotocol sidecar (the machine path: Claude Code, CI bots, custom agents) *and* by humans signing into the dashboard in a browser (the user path). Each key carries an identity, role, adhoc flag, disk quota, and retention ceiling on the server side.

**Why one credential type for both paths.** An agent holding Alice's key is authorised *exactly* as Alice is. If Alice can submit adhoc workflows, so can her Claude Code instance; if Alice cannot shut down the server, neither can her agent. This design means every action — whether it originated from a browser click or an LLM tool call — traces back to a single human user in the audit log. Agents genuinely act **on behalf of** their user, without a separate service-account pattern.

Practical implications:
- **One issuance flow.** The admin issues *one* MCP key per user; the user uses it in the dashboard, the CLI, Claude Code, or any other MCP client. No parallel service credentials.
- **One revocation flow.** Revoking a key cuts off that human's browser sessions, MCP agent sessions, and any script using the key — all at once.
- **One audit trail.** `alice@company.com` appears in the security log for dashboard logins, adhoc submissions, artifact downloads, and every other authenticated action. Role-based accountability, not just credential-based.

There are three distinct actors in the flows below: the **admin** who issues credentials, the **user** who activates and uses them, and the **agent** process (Claude Code, a CI bot, a custom script) that holds the final key on behalf of that user.

### I'm a new user — how do I get a key?

Your admin hands you an **enrollment token** (`enroll_…`) out-of-band — Slack DM, email, in person. The token is single-use and expires within 30 minutes of creation (60 minutes for the first-run bootstrap token).

1. Open the JarvisAgent dashboard in your browser (`https://<host>:8443/`).
2. At the Sign-in dialog, paste the `enroll_…` token into the input field. The dialog auto-switches to **Activate enrollment token** with your token pre-filled — no extra click required.
3. Click **Activate**. Your real MCP API key appears on screen. **It is shown exactly once.**
4. Tick the *"I have saved this MCP API key to a password manager or my Claude Code config"* checkbox. The **Sign in with this key →** button flips from red to green.
5. Copy the key into your password manager (or Claude Code config — see next section). Then click **Sign in with this key →** to enter the dashboard.

If you lose the key, there is no way to recover it — ask the admin for a fresh enrollment token. Don't share your key; treat it like a password.

### I'm a user — how do I use my key with Claude Code?

The MCP sidecar (`mcp/dist/index.js`) reads the key from two sources, in order:

- `J9T_TOKEN` environment variable, *or*
- `J9T_TOKEN_FILE` environment variable pointing to a file whose contents are the raw key (with `chmod 600`, gitignored).

Minimal `.mcp.json` snippet:

```json
{
  "mcpServers": {
    "j9t": {
      "command": "node",
      "args": ["/abs/path/to/jarvisAgent/mcp/dist/index.js"],
      "env": {
        "J9T_URL": "https://localhost:8443",
        "J9T_TOKEN_FILE": "/abs/path/to/.mcp_admin_token"
      }
    }
  }
}
```

Restart Claude Code after editing `.mcp.json`. Verify with the `whoami` MCP tool — it should return your user and role.

### I'm a user — my key is about to expire

MCP keys expire 90 days after activation by default. Within 30 days of expiry, responses to your requests carry an `X-Key-Expires-In: <N>d` header. You can renew **without admin involvement** by calling `POST /api/auth/mcp-keys/self-renew` with your current (still-valid) key — a fresh key is issued, the old one enters a 24-hour grace period, then disables. The same ceiling (role, adhoc flag, quota) carries over; self-renewal cannot escalate privilege.

If your key has already expired, the admin must issue a new enrollment token.

### I'm an admin — how do I onboard a new user?

1. Sign into the dashboard with your admin MCP key.
2. Open **Settings** (gear icon) → **MCP Keys** tab.
3. Click **+ Create enrollment**.
4. Fill in the user's email/username, role (`admin` / `operator` / `viewer`), adhoc flag, disk quota, default retention, and expiry. Click **Create**.
5. Copy the `enroll_…` token shown in the confirmation dialog and share it with the user over a secure channel (Slack DM, email, password manager). You will never see the user's real MCP key — that's by design.
6. Audit log records `Enrollment token created for alice@company.com (operator, adhoc=off) by admin@company.com`.

To **revoke** a key: Settings → MCP Keys → click **Revoke** on the row. Revocation is immediate; subsequent requests with that key receive 401. The revocation is audit-logged.

To **re-enroll** a user (rotate their key or change role/quota): issue a fresh enrollment token. The user activates it, and their previous key enters the 24-hour grace period.

### I'm an admin — how do I deploy scripts?

Scripts under `scripts/` are the only way for adhoc workflows to execute shell / Python code — agents can reference them but cannot upload code. To make a script agent-discoverable:

1. Drop the file into `scripts/` on the j9t host. Shell scripts are `.sh`/`.ps1`; Python modules are `.py` importable as `scripts.<name>`.
2. Add a metadata header so it surfaces in the catalog with a description:

   ```bash
   #!/usr/bin/env bash
   # @jarvis-script
   # @short: One-line description of what the script does
   # @params: ARG1 ARG2 ARG3
   # @description: Longer explanation, possibly wrapping onto
   #   indented continuation lines.
   # @outputs: Free-text description of what the script produces
   ```

3. Make it executable: `chmod +x scripts/my-script.sh` (shell only).
4. In the dashboard, open **Settings → Scripts** and click **Refresh** to trigger an on-demand rescan. No j9t restart required. The MCP `list_scripts` tool and `GET /api/scripts` endpoint will now surface the new script with its metadata; agents submitting adhoc JCWFs can reference it.

Scripts without the `@jarvis-script` marker still appear in the catalog but are flagged — useful for flagging internal helper modules (e.g. `scripts/helpers/*.py`) that agents shouldn't reference directly as task entries.

### I'm an admin — how do agents use adhoc workflows?

An authorised agent (MCP key with `adhoc_enabled`, role `operator`+) composes a JCWF referencing pre-deployed scripts and submits it via `run_adhoc_workflow`. The full loop:

1. **Discover** — `list_scripts` returns the catalog; the agent's LLM picks the scripts it needs.
2. **Compose** — the agent assembles a JCWF canvas (task graph, inputs, outputs) referencing those scripts.
3. **Submit** — `run_adhoc_workflow` stages the JCWF under `_adhoc/<user_slug>/<timestamp>_<counter>_del-<delete-at>/` and kicks off execution. Missing script references are rejected with `400 missing_scripts`.
4. **Monitor** — `get_run_status` polls until the state is terminal.
5. **Enumerate** — `list_run_files` returns the run's outputs with size, content-type, SHA-256, a local filesystem `path` (for agents on the same host), and a `download_url` (for remote agents).
6. **Retrieve** — `get_run_file` streams an artefact by path. Text is returned inline; binary is base64-encoded. Range requests are supported for files over the 10 MB single-response cap.
7. **Cleanup** — based on the submission's cleanup policy: `on_completion` wipes the folder inline; TTL policies (`ttl_1h` / `ttl_24h` / `ttl_48h` / `ttl_72h`) are swept by the reaper thread; `retain` needs admin action.

Every step is audit-logged with the agent's identity, role, and (for file reads) bytes served. Admins can read any user's run folders (`admin_cross_user_read` audit entry); operators see only their own.

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

JarvisAgent uses **JC Workflow** files (`.jcwf`) to describe automation pipelines as directed acyclic graphs (DAGs). A `.jcwf` file is a **zip container** that bundles JSON workflow definitions and any input data files into a single portable package.

A workflow consists of:

- **Tasks** — individual units of work: AI calls, shell commands, Python scripts, internal actions, or sub-workflow invocations. Tasks can run in parallel when their dependencies allow it.
- **Edges** — dependency and dataflow connections between tasks. A task only runs after all its upstream dependencies have completed.
- **Triggers** — how a workflow is started: manually, on a cron schedule, or when a file appears in a watched directory.
- **Filters** — per-item expansion from CSV files, text line lists, or queries. A single task definition can fan out into many parallel instances.
- **Sub-workflows** — nested workflow canvases that group tasks into reusable units. Each sub-workflow is a folder inside the `.jcwf` container with its own JSON task DAG.

### Container format

A `.jcwf` file is a zip archive containing:

- `global.json` — workflow-wide metadata (version, id, label, triggers, defaults).
- `<workflow-name>.json` — the root canvas task DAG.
- Sub-workflow folders — each folder represents a sub-workflow (folder name = display name), containing its own `.json` task DAG.

When loaded, the container is extracted to `workflows/<workflow-name>/`. All task `working_directory` paths are relative to this extracted folder.

### Task types

- **`ai_call`** — Send a prompt (with optional file attachments) to an AI provider and capture the response.
- **`shell`** — Run a shell command or script. Stdout and stderr are captured.
- **`python`** — Call a Python function with structured inputs/outputs.
- **`internal`** — Built-in actions (e.g. file operations).
- **`sub_workflow`** — Execute a child workflow from a sub-folder within the container. The parent task waits for the child to complete.

For the full specification including JSON schema, dataflow mapping, template syntax, filter types, per-item expansion, and container format, see: **doc/JC_Workflow_Specification.md**

## WORKFLOW EDITOR

The visual workflow editor is a React application served by JarvisAgent at:

```
http://localhost:8080/editor
```

Key features:

- **Visual DAG editor** — drag-and-drop nodes, draw dependency and dataflow edges between tasks.
- **Task inspector** — configure task type, parameters, file inputs/outputs, working directory, timeout, and per-task AI interface override.
- **Sub-workflow support** — add sub-workflow nodes that reference nested canvases. Double-click a sub-workflow node to navigate into it.
- **Workflow tree** — the left sidebar shows the sub-workflow hierarchy as a collapsible tree. Click any node in the tree to navigate directly to that canvas.
- **Breadcrumb navigation** — when inside a sub-workflow, a breadcrumb bar above the canvas shows the navigation path. Click any parent in the chain to jump back.
- **Workflow CRUD** — open, save, validate, run, and clean workflows. Workflows are saved as `.jcwf` zip containers.
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

- **MCP API key authentication.** All programmatic endpoints require `Authorization: Bearer mcp_...`. Keys are per-user (identity, role, adhoc flag, disk quota, retention policy) and stored only as SHA-256 hashes in the encrypted `mcp_keys.json.enc`. Provisioning: admin creates an enrollment token (`POST /api/auth/mcp-keys/enroll`); the user exchanges it at `POST /api/auth/mcp-keys/activate` and receives their real key **once**. On j9t's first run with an empty key store, a bootstrap admin enrollment token is logged to stderr. Key comparison uses `CRYPTO_memcmp` for constant-time equality.
- **Dashboard sessions.** Browser users sign in at the login page by pasting their MCP key → `POST /api/auth/login` creates a server-side session with an `HttpOnly + SameSite=Strict` cookie (plus `Secure` when TLS is enabled), 8-hour sliding timeout. `POST /api/auth/logout` destroys the session.
- **Key expiry and self-renewal.** MCP keys expire 90 days after activation by default. Within 30 days of expiry, users can renew themselves at `POST /api/auth/mcp-keys/self-renew` without admin involvement; old keys enter a 24-hour grace period then disable. After expiry, the admin must issue a fresh enrollment token.
- **Failed auth lockout.** 10 failed attempts from the same IP within 5 minutes triggers a 15-minute lockout (HTTP 403 with `Retry-After: 900`).
- **RBAC (Role-Based Access Control).** Three roles with descending privilege:
  - **admin** — full access including shutdown, security logs, MCP key CRUD, and all configuration endpoints.
  - **operator** — run control, workflow monitoring, application logs, adhoc workflow submission (when `adhoc_enabled` is granted).
  - **viewer** — read-only dashboard, workflow list, run status.

  The role travels on the MCP key itself, on the session cookie derived from it, or (in gateway deployments) in the `X-Forwarded-Role` header (default `viewer` when absent).
- **Per-IP rate limiting.** Token bucket algorithm: 100 requests/minute per IP, burst of 20. Rate-limited requests receive HTTP 429 with `Retry-After`.
- **HMAC-SHA256 webhook authentication.** Webhook triggers require a per-workflow secret. Callers must include `X-Webhook-Signature: sha256=<hex>` computed over the raw request body. In Engine mode, webhooks without a configured secret are rejected.
- **WebSocket authentication.** Browser upgrades are validated at the handshake via the session cookie (`.onaccept` hook); no in-band auth message is used.
- **Built-in TLS (HTTPS).** Set `"TlsCert"` and `"TlsKey"` in `config.json` to serve HTTPS (default port 8443). If only one field is set or the files don't exist, j9t refuses to start. When TLS is enabled, HSTS headers are added.
- **Gateway-trusted identity headers.** When deployed behind an API gateway (Kong, AWS API Gateway, Traefik, nginx), configure `"TrustedProxyHeader"` and `"TrustedRoleHeader"` in `config.json`. The gateway handles authentication (OIDC, MFA, SSO) and j9t reads the authenticated user and role from the injected headers.
- **Security audit logging.** All auth events are logged to `log/security.txt` (rotating, 10 MB x 5 files). Logged events: auth success/failure, rate limit triggers, lockout triggers, webhook accept/reject, shutdown requests, and run control actions — all with IP, user identity, and timestamps. Accessible via `GET /api/log/security` (admin role required) and the dashboard's Security tab.
- **Security response headers.** All responses include `Content-Security-Policy`, `X-Frame-Options: DENY`, `X-Content-Type-Options: nosniff`, `Referrer-Policy`, and `Permissions-Policy`.
- **Request body size limit.** Configurable via `"MaxRequestBodyMB"` (default 10 MB). Oversized requests are rejected with HTTP 413 before parsing.

For the complete security model, threat analysis, and operator responsibilities, see **doc/cyber security.md**.

## FILES

- **`config.json`** — Main configuration file (must exist in the working directory).
- **`keys.json.enc`** — Encrypted AI provider credentials (AES-256-GCM, master-password protected).
- **`keys.json`** — Plaintext API keys (development fallback, gitignored).
- **`mcp_keys.json.enc`** — Encrypted MCP API key store (same master password as `keys.json.enc`; only SHA-256 hashes of keys are persisted).
- **`workflows/`** — Directory containing `.jcwf` workflow definition files.
- **`_adhoc/`** — Per-run folders for adhoc workflow submissions (auto-cleaned per the configured retention policy).
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
