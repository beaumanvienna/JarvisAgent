# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

JarvisAgent is a C++ backend + React frontend application for parallel AI-driven automation. It is a workflow engine that coordinates concurrent AI requests with visual workflow editing and multi-platform deployment.

## Build System

The project uses **Premake5** to generate platform-specific build files.

Two editions are available, selected at build time:

| Edition | Flag | Output | Purpose |
|---------|------|--------|---------|
| **j9t Studio** (default) | *(none)* or `--studio` | `bin/Release/jarvisAgent-studio` | Full developer IDE — workflow editor, AI JCWF generation, AI assistant, config editing |
| **j9t Engine** | `--engine` | `bin/Release/jarvisAgent-engine` | Lean production server — no workflow CRUD, no AI tooling, no script writing |

```bash
# Studio edition (default — full developer IDE)
premake5 gmake
make config=release    # → bin/Release/jarvisAgent-studio

# Engine edition (lean production server)
premake5 gmake --engine
make config=release    # → bin/Release/jarvisAgent-engine

# Optional: Enable Tracy profiler
premake5 gmake --tracy            # Studio + Tracy
premake5 gmake --engine --tracy   # Engine + Tracy

# Debug build
make config=debug verbose=1
```

Each edition has its own objdir (`bin-int/studio/` vs `bin-int/engine/`) so switching editions triggers a full rebuild automatically.

For Windows, use `premake5 vs2022` (Studio) or `premake5 vs2022 --engine` (Engine) to generate a Visual Studio 2022 solution.

## React UIs

Two separate React apps that build into the C++ binary's served static files:

```bash
cd dashboard/ui && npm install && npm run build
cd workflow-editor/ui && npm install && npm run build
```

## Running

```bash
# Recommended: use the launcher (auto-manages Python venv):
./jarvisagent.sh              # Studio edition
./jarvisagent.sh --engine     # Engine edition

# Dashboard: https://localhost:8443
# Workflow Editor: https://localhost:8443/editor

# Or run the binary directly:
./bin/Release/jarvisAgent-studio
./bin/Release/jarvisAgent-engine
```

## Testing

Tests are Python scripts that hit the REST API of a running JarvisAgent instance:

```bash
# Terminal 1: start the server
./bin/Release/jarvisAgent-studio

# Terminal 2: run tests
python3 test/run_tests.py --all             # all workflows
python3 test/run_tests.py --workflow NAME   # single workflow
python3 test/run_tests.py --list            # list available workflows

# AI assistant tests
python3 test/assistant/test_assistant.py                          # 28 non-AI tests
python3 test/assistant/test_assistant.py --with-ai               # all 70 tests
python3 test/assistant/test_assistant.py --with-ai --auto-approve # all 70 tests including mutating tools

# Note: Use 'python' instead of 'python3' on Windows and Arch/Manjaro
```

## Code Style

- **Allman** brace style (braces on their own lines after functions/classes/control flow)
- **Member fields**: `m_` prefix + PascalCase (e.g., `m_ThreadPool`, `m_Config`)
- **Column limit**: 125 characters
- **Indent**: 4 spaces
- **Pointer alignment**: Left (`int* ptr`, not `int *ptr`)
- C++20 standard

Format with clang-format using the `.clang-format` config at the root. No automated linting in CI — formatting is manual/IDE-driven.

### Discipline rules (each one came from a real bug — don't relax them)

- **No `default:` arms in `switch` over closed enums we own** (`InterfaceType`, `AuthStyle`, etc.). Either enumerate every case (the `-Wswitch` warning catches missing arms when a variant is added) or `static_assert(NumVariants == N, "extend this switch")`. A `default:` that silently absorbs unknown variants is anti-debugging armor — see the silent-Bearer-fallback bug in `CurlMultiDispatcher` for a real example.
- **Don't duplicate complex-struct construction across files.** If two places build the same `QueryData` / envelope / similar struct from the same inputs, extract a `BuildXxx(...)` helper before a third site appears. Parallel construction sites are guaranteed to skew when fields are added.
- **Failure-path logs are ERROR-level AND mention the runId or workflowId as a literal substring.** The dashboard's Run Analysis filters issues to lines containing this run's identifiers; a fail-path log without an id (or at WARN level) is invisible to it. Subsystems without run context (parsers, signers) return errors via their data types and let the upstream caller — which has the runId in scope — emit the ERROR log. Concrete: `LOG_APP_ERROR("AiRequestPool::Submit: ... run='{}' workflow='{}' task='{}': ...", ...)` not `LOG_APP_WARN("operation failed: %s", err)`.
- **All C++ output goes through `LOG_*` macros**, never `std::cout` / `std::cerr`. The macros land in both the ncurses TUI and `log/log.txt`; raw stream writes only land in one (or are silently swallowed in TUI mode).
- **simdjson is the only JSON library**. Don't add nlohmann or RapidJSON for new capabilities — extend on top of simdjson.
- **Filesystem-touching paths from external strings go through `application/file/pathConfinement.h::ConfineUnderProjectRoot()`**, not local re-implementations. Fail-closed; rejection logs at ERROR with the offending input. Use sites: Python `sys.path` entries, Python `taskWorkingDirectory`, `PythonEnginePool::Initialize`'s `scriptPath`, `WorkflowRuntimeManager::CleanWorkflow`'s 5 `fs::remove*` sites. When a 6th site appears — extend the helper's use-site list, don't write a fresh local copy.
- **Outbound HTTP from workflow context (e.g. completion-callback `callbackUrl`) goes through an SSRF gate**: scheme allowlist (`https://` only), DNS resolution + per-address rejection of loopback / RFC 1918 / link-local / unique-local / multicast / cloud-metadata ranges (incl. IPv4-mapped IPv6 unwrap), TLS verify + no-redirect + protocol allowlist. The reference implementation is `IsCallbackUrlAllowed` in `workflowRuntimeManager.cpp`; cloud-connector requests use `ConnectorHttp` which has its own equivalent. Don't add a new outbound-HTTP surface without one.

## Architecture

### Major Components

| Component | Responsibility | Key Files |
|-----------|---------------|-----------|
| **Engine Core** | Thread pool, logging, event queue, JSON parsing | `engine/engine.h`, `engine/event/` |
| **Workflow Runtime** | DAG-based task execution, JCWF parsing | `application/workflow/workflowJsonParser.h`, `triggerEngine.h` |
| **AI Request Pool** | Parallel AI dispatch over six adapters (OpenAI Chat / OpenAI Responses / Gemini native / Anthropic Messages / Azure OpenAI / AWS Bedrock); auth uniformly via `IAuthSigner` (Bearer / x-api-key / x-goog-api-key / api-key / SigV4) | `application/workflow/aiRequestPool.h`, `aiCallTaskExecutor.h`, `engine/curlWrapper/authSigner.h` |
| **Session Manager** | Queue file monitoring, STNG/CNTX/TASK file assembly | `application/session/` |
| **File Watcher** | Real-time queue folder change detection | `application/file/fileWatcher.h` |
| **Web Server** | REST API + WebSocket, React UI serving (Crow framework) | `application/web/webServer.h` |
| **Workflow Registry** | JCWF workflow CRUD, versioning | `application/workflow/workflowRegistry.h` |
| **Python Engine** | Executes Python scripts embedded in workflows | `application/python/pythonEngine.h` |
| **AI Assistant** | Natural language workflow generation | `application/assistant/` |
| **AI JCWF Service** | AI-driven JCWF generation pipeline (decompose, generate, review) | `application/web/aiJcwfService.h` |
| **JCWF Container** | Zip container read/write for `.jcwf` files (miniz) | `application/workflow/jcwfContainer.h` |
| **Trigger Engine** | Cron, file-watch, webhook, manual triggers | `application/workflow/triggerEngine.h` |
| **Task Executors** | Pluggable executors: shell/Python/AI/internal tasks | `application/workflow/taskExecutor*.h` |

### Data Flow

1. **Trigger Engine** fires workflows on cron / webhook / file-watch / manual events
2. **Workflow Runtime** runs the task DAG, materializing each `ai_call` task's queue folder with **STNG** (settings), **CNTX** (context), **TASK** (instruction), **PROB** (one prompt per fan-out item) inputs
3. **AI Request Pool** dispatches the assembled `AiInvocation` envelope in parallel — disk-first stays preserved (PROV sidecar + transcript still written), but the envelope is the authoritative source of truth, not a file-watcher round-trip
4. The reply lands at `<prob>.output.{txt,json}`; the runtime advances the DAG to dependent tasks
5. **Web Server** exposes REST + WebSocket for the React UI to monitor execution

See `doc/architecture.md` "AI Dispatch Pipeline" for the full diagram.

### Workflow Format (JCWF)

Workflows are `.jcwf` zip containers holding `global.json` (metadata), canvas JSONs, and sub-workflow folders. Key concepts:
- **Template variables**: `{{binding.field}}` substitution in task inputs
- **Per-item fan-out**: CSV/text filters spawn parallel AI calls per item
- **Task dependencies**: Explicit `dependsOn` edges enabling serial + parallel execution
- **Sub-workflows**: Nested canvas pages with their own task DAGs
- **Disk-first design**: All inputs, outputs, and intermediates stored on disk

See `doc/JC_Workflow_Specification.md` for the full format definition.

### Key Documentation

- `doc/JC_Workflow_Specification.md` — Complete JCWF format and execution model
- `doc/api-endpoints.md` — REST API reference
- `doc/architecture.md` — Detailed architecture overview, including the **Key Design Decisions** table (the "why" behind non-obvious choices)
- `doc/jarvisagent.md` — User manual / `config.json` reference
- `doc/cyber security.md` — Threat model, MCP key lifecycle, master-password handling
- `mcp/README.md` — MCP tool surface (run, configure, artifact retrieval)
- `integration/README.md` — Webhook triggers, n8n integration, HMAC signing
- `docker-compose.example.yml` — Optional dev mocks (`aoai-api-simulator` for Azure OpenAI testing, LocalStack Hobby tier for Bedrock testing) — both kept commented; uncomment to run

### Vendored Dependencies

All third-party libraries are in `vendor/`: libcurl + openssl (HTTP/HTTPS), spdlog (logging), simdjson (JSON parsing), pdcursesmod (terminal UI), crow (REST framework), asio (async I/O), thread-pool, tracy (optional profiler), date (timezone).
