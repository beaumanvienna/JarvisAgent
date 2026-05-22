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
| **Workflow CRUD** | Full | Not available (compile-time removed) |
| **AI assistant** | Full | Not available (compile-time removed) |
| **AI JCWF generation** | Full | Not available (compile-time removed) |
| **Attack surface** | Full feature set | Minimal — runtime + monitoring only |

**Shared security posture (both editions):** MCP API key + session-cookie + optional gateway-header auth funnel; 3-role RBAC (`admin`/`operator`/`viewer`); per-IP rate limiting (100 req/min, burst 20); failed-auth lockout (10 → 15-min IP ban); audit log at `log/security.txt` (rotating, 10 MB × 5); configurable request body cap (default 10 MB); CSP / X-Frame-Options / HSTS response headers. The anonymous-localhost bypass that Studio used pre-§5i was removed in 2026-05; every endpoint now goes through the same auth gate regardless of edition. See [Security](#security) and [doc/api-endpoints.md](api-endpoints.md) Authentication.

Studio is designed for single-developer use on a local machine. Engine is designed for
production deployments behind an API gateway. The auth/RBAC/audit machinery is the same — Engine just drops the editing surface so the attack surface is smaller. See [Security](#security) for details.

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

For detailed install/uninstall instructions for each package format, see **[INSTALL.md](../INSTALL.md)**.
To build from source, see **[DEVELOPMENT.md](../DEVELOPMENT.md)**. Packaging internals live in
**[packaging/packaging.md](../packaging/packaging.md)**.

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
- **`"max inflight ai calls"`** — (number) Maximum concurrent AI requests dispatched via HTTP/2. Decoupled from thread pool size since requests are multiplexed on a single I/O thread. Default: 1000. Valid range: 1–10000.
- **`"max_ai_calls_per_jcwf"`** — (number) Per-run cap on the total AI calls a single JCWF run can dispatch. `0` = unlimited. Default: 0. Tasks exceeding the cap fail fast (`AI call cap exceeded`). Set to a finite value (e.g. 1000) for adhoc / multi-tenant deployments to bound a single workflow's AI-budget consumption.
- **`"max_per_item_fan_out"`** — (number) Cap on the children spawned by a single per-item filter evaluation. `0` = unlimited. Default: 10000. Guards against DoS via a malicious or accidental filter that returns millions of items (each becomes a task child + downstream dispatch). When exceeded, the parent task fails with an ERROR-logged `runId`/`workflowId`/`taskId`.
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
    - `API4` — Anthropic Messages API (uses `x-api-key` + `anthropic-version: 2023-06-01` headers, `/v1/messages` endpoint; Claude Haiku / Sonnet / Opus).
    - `API5` — AWS Bedrock (SigV4-signed; URL composed as `{base}/model/{modelId}/invoke`; body shape dispatches on `modelId` prefix: `anthropic.claude-*` → Anthropic-style, `meta.llama*` → Llama-native, `amazon.titan-*` / `amazon.nova-*` → Titan/Nova; reply parser sniffs the response shape and delegates).
    - `API6` — Azure OpenAI (uses `api-key:` header; body identical to API1; the deployment URL is the full per-deployment URL, e.g. `https://{resource}.openai.azure.com/openai/deployments/{deployment}/chat/completions?api-version={ver}`).
  - **`"max_context_tokens"`** — (integer, optional) Advisory context-window size for this interface. When set, j9t warns if an `ai_call` prompt is estimated to exceed it. Typical values: OpenAI GPT-4-family = 128000; OpenAI GPT-5-family = 200000; Google Gemini 2.5 = 1000000; Anthropic Claude = 200000.
  - **`"default_output_tokens"`** — (integer, optional) Default max output tokens used by the size-aware request budget when an envelope's `m_MaxTokens` isn't set. Default: 4096.
  - **`"name"`** — (string) Human-readable name for this interface. Auto-generated from URL domain + model if omitted.
  - **`"description"`** — (string) Optional description of this interface.
  - **`"key_name"`** — (string) Name of the API key provider to use from the encrypted keys file (e.g. "openai", "google", "anthropic").
  - **`"is_mock"`** — (bool, optional) When `true`, the dispatcher routes calls to this interface through MockTransport (fixture replay) instead of LiveTransport (real HTTPS).  Useful for hermetic tests, demos without provider credit, and CI.  Admin-only — same access surface as `api_key`.  Requires `fixture_path` to be set; the parser rejects `is_mock: true` without a non-empty `fixture_path` and marks the interface InvalidAPI.
  - **`"fixture_path"`** — (string, optional unless `is_mock: true`) Path to the on-disk fixture file MockTransport reads as the response body.  The file must be a full API-shaped response (e.g. an OpenAI chat completion JSON for `api_type: "API1"`); the configured `api_type`'s ReplyParser parses it identically to a real response.  Hardening: paths are resolved against the project root via `ConfineUnderProjectRoot` and rejected if they escape (absolute outside the root, symlink target outside, `..` traversal that lands outside).  Fixture size is capped at 10 MiB; oversized files fail closed.  An optional sibling `<fixture>.meta.json` overrides HTTP status + headers and pins SigV4 timing for KAT tests — `http_status` must be in `[200, 599]`; only `Content-Type` and `Retry-After` headers are allowlisted (others dropped with WARN); an optional `x_amz_date_override` field (format `"YYYYMMDDTHHMMSSZ"`) feeds `QueryData::m_AmzDateOverride` so SigV4 paths produce a byte-deterministic Authorization header (consumed by `test/dispatch/test_bedrock_sigv4.py`).  PROV sidecar carries `"mocked": true` + the resolved `fixture_path` so post-mortem tooling distinguishes mock dispatches from live ones.
  - **`"rate_limit"`** — (object, optional) Per-interface adaptive rate-limit + size-aware in-flight budget knobs. All sub-fields optional; missing fields fall back to per-`InterfaceType` defaults shipped in the binary. See **Rate-limit configuration** below for the schema and tuning examples.

### Rate-limit configuration

Every interface has an associated adaptive controller that decides how aggressively to dispatch requests, plus a size-aware in-flight budget that bounds each request's curl timeout. Both are tuned by the optional `rate_limit` block on the interface:

```jsonc
{
    "API": "API4",
    "url": "https://api.anthropic.com/v1/messages",
    "model": "claude-sonnet-4-6",
    "key_name": "Anthropic",
    "rate_limit": {
        "initial_concurrency_probe": 4,
        "max_concurrency": 48,
        "max_retries_429": 10,
        "max_retries_transient": 2,
        "base_retry_ms": 1000,
        "request_budget": {
            "per_1k_input_token_seconds": 0.5,
            "per_1k_output_token_seconds": 5.0,
            "fixed_overhead_seconds": 5.0,
            "safety_margin_factor": 4.0,
            "min_seconds": 60.0,
            "max_seconds": 1800.0
        }
    }
}
```

**Concurrency / retry knobs:**

- `initial_concurrency_probe` — starting AIMD cap before the controller has observed any response. Default per `InterfaceType`: Anthropic 4, OpenAI 8, Empty 4. Set to 1 for a Tier-1 Anthropic account; raise to 16+ for Tier-3+.
- `max_concurrency` — hard ceiling AIMD growth never crosses, regardless of how many clean completions accumulate. Default 48 (the HTTP/2 stream cap). Set lower to pace burn rate on cost-capped accounts (this is the only cost-shaping knob in 1.0). **Also feeds the request-budget formula** below — see the size-aware budget section.
- `max_retries_429` — number of attempts after a 429 before giving up. Default 10 (controller's predictive gating means real 429s are rare in practice).
- `max_retries_transient` — attempts after a transient HTTP error (400/500/502/503). Default 2.
- `base_retry_ms` — first retry delay; subsequent retries use exponential backoff `base * 2^n`. Default 1000.

**Size-aware request budget** — every request gets a curl timeout (`CURLOPT_TIMEOUT_MS`) computed from the formula:

```
seconds = (input_tokens / 1000 × per_1k_input_token_seconds)
        + (max_output_tokens / 1000 × per_1k_output_token_seconds)
        + fixed_overhead_seconds
seconds *= max_concurrency        # worst-case queue-depth multiplier
seconds *= safety_margin_factor   # token-rate variance headroom
seconds  = clamp(seconds, min_seconds, max_seconds)
```

The `max_concurrency` multiplier handles the case where a backend serializes (e.g., a local ollama daemon on a single GPU): a request that lands at position N in the backend's queue takes N × single-stream time. Multiplying by `max_concurrency` (the configured ceiling on simultaneous in-flight requests) guarantees the timeout covers the worst-case queue position. For truly-parallel backends (cloud providers like OpenAI / Anthropic) the multiplier over-allocates timeout harmlessly — the only side effect is slower detection of genuinely hung requests, which is rare and worth trading for never-timeouts-from-contention reliability. No per-user knob to fiddle with.

Curl's timeout only counts time *on the wire* — inbox waits, controller throttling, and retry-queue backoffs don't burn the budget. Each retry creates a fresh easy handle with a fresh budget.

**Default rate at which providers generate output** (the dominant term for typical AI workloads):

| Provider tier | Approx. output rate | Recommended `per_1k_output_token_seconds` |
|---|---|---|
| Anthropic Claude Haiku 4.5 | ~150 tok/s | 1.0 |
| OpenAI gpt-4o-mini, gpt-5-nano | ~120 tok/s | 1.5 |
| OpenAI gpt-4o, gpt-4.1 | ~80 tok/s | 2.5 |
| Google Gemini 2.5 Flash | ~70 tok/s | 3.0 |
| Anthropic Claude Sonnet 4.6 | ~70 tok/s | 5.0 |
| Anthropic Claude Opus 4.7 | ~30 tok/s | 12.0 |

Shipped defaults (`per_1k_output=5.0`, `safety_margin=4.0`, `min=60s`, `max=1800s`) are calibrated to absorb the worst-case stack of (slowest cloud provider × full queue depth × token-rate variance). Fast providers finish well within the floor with no harm done; serializing local backends (e.g. ollama) get a budget that scales with `max_concurrency` so contention-induced queueing never trips the timeout. The 30 min `max_seconds` ceiling is the safety stop for pathological combinations (very large outputs × max queue depth × slow provider) — at that point the request is almost certainly hung rather than legitimately progressing.

**Tuning examples:**

*Tier-1 Anthropic (free / starter, ~5 RPM Sonnet):*
```jsonc
"rate_limit": { "initial_concurrency_probe": 1, "max_concurrency": 4 }
```

*Tier-3 Anthropic (production, ~50 RPM Sonnet):*
```jsonc
"rate_limit": { "initial_concurrency_probe": 8, "max_concurrency": 32 }
```

*Cost-capped account — pace burn at ~10 in-flight max regardless of provider tier:*
```jsonc
"rate_limit": { "max_concurrency": 10 }
```

*Workflow expects very long Opus responses (8K-12K tokens):*
```jsonc
"rate_limit": {
    "request_budget": {
        "per_1k_output_token_seconds": 15.0,
        "min_seconds": 120.0,
        "max_seconds": 3600.0
    }
}
```

*Local serializing backend (ollama, llama.cpp, vLLM single-GPU) — no extra knobs needed:* the default `max_concurrency: 48` multiplier in the budget formula automatically gives each request enough wall-clock to clear a full queue. If your hardware can sustain higher concurrent throughput (e.g. `OLLAMA_NUM_PARALLEL=8`), lower `max_concurrency` to a value `j9t` won't exceed simultaneously — the budget shrinks proportionally and you get faster hung-request detection.

**Verifying tuning** — `GET /api/debug/signals` (debug builds only) exposes per-`(host, modelFamily)` controller state at `dispatcher_controllers[]`: current AIMD cap, streak since last 429, last observation (remaining requests / tokens, reset times), last consumed input/output tokens. Use this to confirm the controller is doing what you expect before scaling up.

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

The `@jarvis-script` marker is what registers a file. Files in `scripts/` without the marker are not catalogued and cannot be referenced by workflow tasks — for **Python** tasks specifically, the runtime allowlist in `PythonEngine` rejects unregistered modules at import time, so an internal helper that lacks the marker is invisible both to the catalog and to the import path. To keep helper modules importable by registered scripts but hidden from agents, place them in a sub-package (`scripts/helpers/`) and import them only from registered scripts above — `scripts/helpers/*.py` reached via relative import inside a registered `scripts/foo.py` works without needing its own marker.

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
2. Configure one or more AI interfaces: set the API endpoint URL, model name, parser type, and select which key provider to use. Use `API1` for OpenAI and compatible providers, `API2` for OpenAI Responses API (GPT-5+), `API3` for Google Gemini native, `API4` for Anthropic Messages (Claude), `API5` for AWS Bedrock, or `API6` for Azure-hosted OpenAI deployments.
3. Set the default interface index or override per-task in workflow definitions.

Supported providers include OpenAI (API1 or API2), Google Gemini (API1 via OpenAI-compat endpoint, or API3 for the native endpoint),
Anthropic (API4 native), Ollama and any provider offering an OpenAI-compatible chat completions API (API1).
The OpenAI Responses API (GPT-5+) uses API2.

**Self-hosted example — Ollama on localhost** (same pattern works for LM Studio, llama.cpp server, vLLM, text-generation-webui):

```json
{
  "name": "ollama/llama3.1/API1",
  "url": "http://localhost:11434/v1/chat/completions",
  "model": "llama3.1",
  "API": "API1",
  "key_name": "ollama"
}
```

Register an `ollama` provider in the KeyManager with any non-empty string as the API key — Ollama itself ignores the bearer, but the j9t dispatcher requires one.

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
- **Two-tier rate limiting.** Token-bucket policy split by authentication state. Pre-auth (per-IP) is tight — 100 req/min, burst 20 — and applies to unauthenticated or credential-rejected traffic. Authenticated (per-user) is generous — 1200 req/min, burst 200 — and applies once an MCP key or session validates. Both tiers return HTTP 429 with `Retry-After`; the security log distinguishes them via `rate_limited_preauth` vs `rate_limited_authenticated`.
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
