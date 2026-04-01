# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

JarvisAgent is a C++ backend + React frontend application for parallel AI-driven automation. It is a workflow engine that coordinates concurrent AI requests with visual workflow editing and multi-platform deployment.

## Build System

The project uses **Premake5** to generate platform-specific build files.

Two editions are available, selected at build time via the `--studio` flag:

| Edition | Flag | Purpose |
|---------|------|---------|
| **j9t Engine** (default) | *(none)* | Lean production server — no workflow CRUD, no AI tooling, no script writing |
| **j9t Studio** | `--studio` | Full developer IDE — workflow editor, AI JCWF generation, AI assistant, config editing |

```bash
# Engine edition (default — production server)
premake5 gmake
make config=release

# Studio edition (developer IDE — full features)
premake5 gmake --studio
make config=release

# Outputs (both → same binary name, different sizes)
bin/Release/jarvisAgent

# Optional: Enable Tracy profiler
premake5 gmake --tracy          # Engine + Tracy
premake5 gmake --studio --tracy # Studio + Tracy

# Debug build
make config=debug verbose=1
```

For Windows, use `premake5 vs2022` (Engine) or `premake5 vs2022 --studio` (Studio) to generate a Visual Studio 2022 solution.

## React UIs

Two separate React apps that build into the C++ binary's served static files:

```bash
cd dashboard/ui && npm install && npm run build
cd workflow-editor/ui && npm install && npm run build
```

## Running

```bash
./bin/Release/jarvisAgent
# Dashboard: http://localhost:8080
# Workflow Editor: http://localhost:8080/editor

# Or use the launcher (auto-manages Python venv):
./jarvisagent.sh
```

## Testing

Tests are Python scripts that hit the REST API of a running JarvisAgent instance:

```bash
# Terminal 1: start the server
./bin/Release/jarvisAgent

# Terminal 2: run tests
python test/run_tests.py --all             # all workflows
python test/run_tests.py --workflow NAME   # single workflow
python test/run_tests.py --list            # list available workflows

# AI assistant tests
python test/assistant/test_assistant.py                          # 28 non-AI tests
python test/assistant/test_assistant.py --with-ai               # all 70 tests
python test/assistant/test_assistant.py --with-ai --auto-approve # all 70 tests including mutating tools
```

## Code Style

- **Allman** brace style (braces on their own lines after functions/classes/control flow)
- **Member fields**: `m_` prefix + PascalCase (e.g., `m_ThreadPool`, `m_Config`)
- **Column limit**: 125 characters
- **Indent**: 4 spaces
- **Pointer alignment**: Left (`int* ptr`, not `int *ptr`)
- C++20 standard

Format with clang-format using the `.clang-format` config at the root. No automated linting in CI — formatting is manual/IDE-driven.

## Architecture

### Major Components

| Component | Responsibility | Key Files |
|-----------|---------------|-----------|
| **Engine Core** | Thread pool, logging, event queue, JSON parsing | `engine/engine.h`, `engine/event/` |
| **Workflow Runtime** | DAG-based task execution, JCWF parsing | `application/workflow/workflowJsonParser.h`, `triggerEngine.h` |
| **AI Request Pool** | Parallel AI API dispatch (OpenAI, Gemini) | `application/workflow/aiRequestPool.h`, `aiCallTaskExecutor.h` |
| **Session Manager** | Queue file monitoring, STNG/CNTX/TASK file assembly | `application/session/` |
| **File Watcher** | Real-time queue folder change detection | `application/file/fileWatcher.h` |
| **Web Server** | REST API + WebSocket, React UI serving (Crow framework) | `application/web/webServer.h` |
| **Workflow Registry** | JCWF workflow CRUD, versioning | `application/workflow/workflowRegistry.h` |
| **Python Engine** | Executes Python scripts embedded in workflows | `application/python/pythonEngine.h` |
| **AI Assistant** | Natural language workflow generation | `application/assistant/` |
| **Trigger Engine** | Cron, file-watch, webhook, manual triggers | `application/workflow/triggerEngine.h` |
| **Task Executors** | Pluggable executors: shell/Python/AI/internal tasks | `application/workflow/taskExecutor*.h` |

### Data Flow

1. **File Watcher** detects changes in the queue folder
2. **File Categorizer** classifies files by type: STNG (settings), CNTX (context), TASK, REQ (requirements)
3. **Session Manager** assembles the environment (STNG + CNTX + TASK files)
4. **AI Request Pool** dispatches AI queries in parallel (one per requirement file)
5. **Workflow Runtime** executes DAG tasks (shell/Python/AI/internal), managing dependencies
6. **Web Server** exposes REST API + WebSocket for the React UI to monitor execution
7. **Trigger Engine** fires workflows on schedule/webhook/file-watch events

### Workflow Format (JCWF)

Workflows are JSON files defining a DAG of tasks with:
- **Template variables**: `{{binding.field}}` substitution in task inputs
- **Per-item fan-out**: CSV/text filters spawn parallel AI calls per item
- **Task dependencies**: Explicit `dependsOn` edges enabling serial + parallel execution
- **Disk-first design**: All inputs, outputs, and intermediates stored on disk

See `doc/JC_Workflow_Specification.md` for the full format definition.

### Key Documentation

- `doc/JC_Workflow_Specification.md` — Complete JCWF format and execution model
- `doc/api-endpoints.md` — REST API reference
- `doc/architecture.md` — Detailed architecture overview
- `integration/README.md` — Webhook triggers, n8n integration, HMAC signing

### Vendored Dependencies

All third-party libraries are in `vendor/`: libcurl + openssl (HTTP/HTTPS), spdlog (logging), simdjson (JSON parsing), pdcursesmod (terminal UI), crow (REST framework), asio (async I/O), thread-pool, tracy (optional profiler), date (timezone).
