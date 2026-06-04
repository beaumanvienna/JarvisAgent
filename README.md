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

**Orchestrate and scale AI-driven operations across your organization.**

j9t is a platform for scaling AI-driven work. It transforms AI calls, Python, shell scripts, and native C++23 into visual workflows that execute hundreds of tasks in parallel — enabling teams to automate complex processes and accelerate analysis, while integrating directly with their existing cloud systems and data sources.

A modern backend with multithreading, HTTP/2 multiplexing, and an adaptive rate limiter that pushes AI to the max ensures fast execution whether running locally or in the cloud. On top of this engine, j9t provides a React-based workflow editor with AI-assisted generation, structured outputs and automatic retry on failure for predictable automation, 14 cloud connectors across 5 categories, and MCP integration with external AI systems.

Workflows fire on cron schedules, file-watch events, HMAC-signed webhooks, or on demand — the same pipeline can run once from a button, every hour, or whenever a file lands in a folder.

**Use cases** — a single workflow can:

- Score 60 portfolio positions in seconds and roll them into one summary
- Triage a backlog of GitHub / Jira / Redmine issues into bug / feature / question buckets with automatic assignee routing
- Assess procurement requirements against a platform spec and push the verdicts back into Polarion
- Grade a classroom of quizzes and write the grades back to a Google Sheet
- Generate a diagram-rich troubleshooting PDF from schema-validated AI calls
- Describe a new workflow in plain English and let the assistant generate, validate, and auto-fix it with `branch-on-error` until it's valid

**Why these chained pipelines beat a single prompt.** Breaking a hard problem into a DAG of small AI calls gives each call a tighter scope — and higher accuracy — than one monolithic prompt. And because any step can be a compiler, `make`, a linter, or a test runner, deterministic tools close the loop: a failed `make` feeds its error back into the next AI call, which rewrites the broken file and loops until green. The same agent-loop pattern coding assistants use — encoded as a reusable, auditable pipeline your team designs once and reruns on schedule.

A dual-edition architecture lets organizations move from prototyping to secure, controlled deployment. Both editions are included in every package:

- **j9t Studio** — visual workflow editor with AI generation, explain, and auto-fix; AI assistant with 31 tools; workflow versioning and live debugging. Same auth posture as Engine: every browser session and MCP client requires an MCP API key.
- **j9t Engine** — lean production runtime. Workflow CRUD / AI assistant / AI JCWF generation are removed at compile time; everything else (run control, settings admin, log analysis, monitoring) is reachable via MCP key and role-gated (admin/operator/viewer). TLS, HMAC webhooks, rate limiting, audit log, optional gateway-header cross-check. Ready for private cloud or behind an API gateway.

**Current version: 0.8.6** — working towards **beta 0.95**, the first major baseline subject to regression testing across all packaging targets.

---

## Screenshots

| Dashboard | Workflow Editor | AI Assistant |
|:---------:|:---------------:|:------------:|
| ![Dashboard](example/screenshot_dashboard.png) | ![Workflow Editor](example/screenshot_workflow_editor.png) | ![AI Assistant](example/screenshot_ai_assistant.png) |

---

## Supported AI Backends

JarvisAgent talks to AI providers through six interface adapters, covering every major hosted provider, every common self-hosted runtime, and the two enterprise-cloud platforms. You pick an interface in `config.json` by setting `API` to one of `API1`/`API2`/`API3`/`API4`/`API5`/`API6`.

| Adapter | Endpoint | Providers that work today |
|---|---|---|
| **API1** — OpenAI Chat Completions | `POST /v1/chat/completions` | **OpenAI** (gpt-4.1, gpt-4o, gpt-4-turbo, gpt-5, mini variants) · **Google Gemini** (OpenAI-compat mode) · **Groq**, **Together AI**, **Fireworks**, **DeepInfra**, **Perplexity**, **xAI Grok**, **Mistral Platform**, **GitHub Models**, **OpenRouter** · self-hosted: **Ollama**, **LM Studio**, **llama.cpp server**, **vLLM**, **text-generation-webui** |
| **API2** — OpenAI Responses | `POST /v1/responses` | OpenAI (Responses API — newer endpoint, used for sequential chunk throughput) |
| **API3** — Gemini native | `POST /v1beta/models/{model}:generateContent` | Google Gemini (native endpoint with `x-goog-api-key` auth) |
| **API4** — Anthropic Messages | `POST /v1/messages` | Anthropic Claude (Haiku / Sonnet / Opus, all generations with 200 K context) |
| **API5** — AWS Bedrock | `POST /model/{modelId}/invoke` (SigV4-signed) | Bedrock-hosted **anthropic.claude-***, **meta.llama***, **amazon.titan-***, **amazon.nova-*** model families |
| **API6** — Azure OpenAI | `POST /openai/deployments/{deployment}/chat/completions?api-version={ver}` | Azure-hosted OpenAI deployments (resource-scoped URLs, `api-key:` header) |

Self-hosted runtimes (Ollama, LM Studio, llama.cpp, vLLM) plug into API1 — see [User Manual](doc/jarvisagent.md) for the config shape. Context-window handling and chunking behavior are documented in [doc/architecture.md](doc/architecture.md).

---

## Cloud Integrations

Workflows read from and write to external systems through a unified `ICloudConnector` framework. Credentials are stored encrypted, never appear in workflow files, and a single connection definition is reused across tasks.

| Category | Systems |
|---|---|
| Object storage | S3 (+ MinIO/R2/Wasabi), Azure Blob, Google Cloud Storage |
| Databases | PostgreSQL, Snowflake |
| ALM | Polarion, Jira, GitHub, Redmine |
| Collaboration | OneDrive, Google Sheets |
| Messaging | Slack, Email (SMTP/IMAP) |

**Why it matters:** the same automation pipeline can pull data from where it already lives, run AI analysis on it, and push results back into the systems your teams already use — no manual export/import, no separate integration scripts, no vendor lock-in.

Typical round-trip: **read from cloud → fan out per item → AI processes each → write results back**.

An **MCP sidecar** also exposes workflows to MCP clients such as Claude Code.

See [doc/cloud-integration.md](doc/cloud-integration.md) for the full architecture and per-connector details.

---

## Documentation

- **[INSTALL.md](INSTALL.md)** — install pre-built packages on all platforms
- **[DEVELOPMENT.md](DEVELOPMENT.md)** — build from source, dependencies, editions, running
- **[User Manual](doc/jarvisagent.md)** — MCP key enrollment, admin tasks, scripts, adhoc workflows (also available as `man jarvisagent` on Linux/macOS)
- [doc/architecture.md](doc/architecture.md) — system architecture and runtime layers
- [doc/JC_Workflow_Specification.md](doc/JC_Workflow_Specification.md) — complete JCWF format
- [doc/cloud-integration.md](doc/cloud-integration.md) — cloud connector framework
- [doc/api-endpoints.md](doc/api-endpoints.md) — REST API reference
- [doc/cyber security.md](doc/cyber%20security.md) — Engine security model
- [integration/README.md](integration/README.md) — webhook triggers, n8n integration, HMAC signing

---

## Example Workflows

| Workflow | Highlights |
|----------|------------|
| [Go-Kart Compliance Check](example/workflows/goKartComplianceCheck.md) | Polarion integration, per-item fan-out, `{{template}}` substitution |
| [Portfolio Dividend Analysis](example/workflows/portfolioDividendAnalysis.md) | CSV filter, 60-position fan-out, glob aggregation |
| [AI Car Maintenance Pipeline](example/workflows/aiCarMaintenancePipeline.md) | Multi-stage pipeline with AI categorization |
| [Hamburg Tourist Day Planner](example/workflows/hamburg-tourist-day-planner.md) | Webhook trigger, n8n integration, HMAC, callback |
| Cloud round-trip demos | [Polarion](example/workflows/goKartComplianceCheck.md), [PostgreSQL](example/workflows/postgresDemo.md), [S3](example/workflows/s3UploadDownloadDemo.md), [Email](example/workflows/emailDemo.md), [GitHub](example/workflows/gitHubIssueDemo.md), [Jira](example/workflows/jiraIssueDemo.md), [Slack](example/workflows/slackQAndABot.md), [OneDrive](example/workflows/oneDriveUploadDownloadDemo.md), [Snowflake](example/workflows/snowflakeQueryDemo.md), [Google Sheets](example/workflows/sheetsQuizGrader.md), [Azure Blob](example/workflows/azureBlobDemo.md), [GCS](example/workflows/gcsDemo.md), [Redmine](example/workflows/redmineTriageBot.md) |

---

## Quick Start

See **[INSTALL.md](INSTALL.md)** for full installation. Fastest path:

**Prerequisites:** Docker (Docker Desktop on macOS/Windows, or Docker Engine on Linux).

```bash
git clone https://github.com/beaumanvienna/JarvisAgent.git
cd JarvisAgent
./scripts/run-docker.sh              # Linux / macOS
scripts\run-docker.ps1               # Windows (PowerShell)
```

The helper script pulls `ghcr.io/beaumanvienna/jarvisagent:latest` and starts the container with a persistent data directory at `~/JarvisAgent`.

Then open the dashboard at `http://localhost:8080` (or `https://localhost:8443` with TLS) and the workflow editor at `/editor`.

The image is published for `linux/amd64` and `linux/arm64` — runs natively on Intel/AMD hosts, Apple Silicon (via Docker Desktop), and ARM Linux.

---

## Contributing

Contributions are welcome. Please enable **clang-format** in your IDE. Coding style is **Allman**, and member fields of structs and classes use the `m_` + PascalCase convention.

---

GPL-3.0 License © 2026 JC Technolabs
