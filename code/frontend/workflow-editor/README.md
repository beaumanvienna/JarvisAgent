# Workflow Editor

Visual workflow editor for j9t — a React + TypeScript single-page app served by the C++ backend at `http://localhost:8080/editor`.

## Tech Stack

| Layer | Technology |
|-------|-----------|
| UI framework | React 18 + TypeScript |
| Graph canvas | React Flow 11 |
| Terminal | xterm.js 6 |
| Build tool | Vite 7 |
| Hosting | Served as static files by the Crow web server |

## Getting Started

```bash
cd code/frontend/workflow-editor/ui
npm install
npm run build      # production build (output served by the C++ binary)
npm run dev         # Vite dev server on localhost:5173 (hot reload)
```

Then start the backend with `./jarvisagent.sh` and open `http://localhost:8080/editor`.

## Directory Layout

```
workflow-editor/
  ui/
    src/
      editor/         # Core canvas — TaskNode, FilterNode, BranchNode,
                      #   WorkflowEditorView, graph<->JCWF converters
      views/          # Page views — WorkflowList, AiManager, Providers,
                      #   Connections, Assistant
      components/     # Reusable UI — SettingsModal, StatusLeds,
                      #   VersionHistoryModal, TemplateBrowser, etc.
      api/            # REST client modules (workflows, aiInterfaces,
                      #   providers, connections, keys, config, versions)
      hooks/          # Custom hooks (useStatusWebSocket,
                      #   useAssistantWebSocket)
      jcwf/           # JCWF type definitions
      templates/      # Starter workflow templates
      assets/         # Logo and static assets
    index.html
    vite.config.ts
    tsconfig.json
    package.json
  todo.md
  README.md
```

## Features

### Workflow Editing
- DAG canvas with custom task nodes, dependency edges, and auto-layout
- Node types: task (shell, Python, AI call, internal), filter, branch, sub-workflow
- Dataflow wiring (dashed green edges with `df:` prefix)
- Template browser with starter workflows
- Undo/redo, box select (Shift+drag), dirty indicator
- Editor layout persistence (node positions saved in JCWF)
- Workflow versioning with history browser and restore

### Task Inspector
- Per-type field editing (shell command, Python module/function, AI prompt, etc.)
- Queue binding editor (STNG/TASK/CNTX/PROB sections)
- Filter builder dialog (csv, text_lines, query, polarion_query)
- File inputs/outputs editor, inputs/outputs slot editor
- Template variable autocomplete (`{{binding.field}}`)
- Per-task timeout, expose_error_signal toggle

### Triggers & Validation
- Trigger editing (auto, manual, cron, file_watch)
- Backend-authoritative validation with severity tiers (A-D)
- Script path validation with inline warnings
- Broken JCWF visibility with error badges

### Runtime Monitoring
- Live run state via WebSocket (running/failed/skipped/paused badges)
- Stop / Pause / Resume controls
- Shell and Python stdout/stderr capture (hover tooltip + side panel)
- Status LEDs in header (connection, queries in flight, run state)

### AI Tooling
- JCWF generation assistant (decompose -> generate -> scripts -> review -> validate -> fix)
- Workflow explain button
- Fix Script button (sends failed script + stderr to AI for repair)
- AI Assistant terminal with 31 tools, slash commands, ghost-text completion

### Dashboard
- Log viewer with virtual scroll, delta polling, search
- Run analysis panel with multi-run cycling and issue filtering
- Security log viewer (auth events, rate limits, lockouts)

### Integration
- n8n webhook integration with HMAC-SHA256 signing
- Sub-workflow navigation with breadcrumb trail and tree view
- AI interface selector for JCWF generation pipeline

## Architecture

```
+-----------------------------+
| React + React Flow (UI)     |
|  - Workflow Editor Canvas   |
|  - AI Assistant Terminal    |
|  - Dashboard & Log Viewer   |
+-------------+---------------+
              |
      REST + WebSocket
              |
+-------------v---------------+
| Crow Web Server (C++)       |
|  - Workflow CRUD API        |
|  - Validation API           |
|  - Run Control API          |
|  - WebSocket Broadcasts     |
+-------------+---------------+
              |
+-------------v---------------+
| j9t Runtime                 |
|  - WorkflowRegistry         |
|  - WorkflowRuntimeManager   |
|  - TriggerEngine            |
+-----------------------------+
```

### Design Principles

- **JCWF is the single source of truth.** The editor never invents runtime behavior.
- **Round-trip fidelity.** Load -> Save preserves every field, even those the editor does not yet understand.
- **Progressive disclosure.** Show fields relevant to the selected task type.
- **Backend-authoritative validation.** The C++ validator is definitive; the UI displays results.
