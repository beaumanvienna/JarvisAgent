# JarvisAgent – Web-Based Workflow Editor (React Flow)
## Software Development Plan

This document describes a **concrete, implementation-oriented development plan** to add a
**web-based workflow editor** to JarvisAgent using **React Flow**, fully compliant with the
**JC Workflow Specification (JCWF)** and tightly integrated with JarvisAgent’s existing
Crow-based web server, workflow runtime, and monitoring system.

---

## 1. Goals and Non-Goals

### Goals
- Provide an **in-dashboard visual workflow editor** similar in spirit to [**n8n**](https://github.com/n8n-io/n8n).
- Use **React Flow** for node graph editing (nodes, edges, layout, interactions).
- Treat **[JCWF JSON](https://github.com/beaumanvienna/JarvisAgent/blob/main/doc/JC_Workflow_Specification.md) as the canonical runtime format** (parallel execution format).
- Support:
  - Import existing workflows
  - Create workflows from scratch
  - Create workflows from templates
  - Edit workflows visually
  - Validate workflows against [JC Workflow File Format™ (JCWF)](https://github.com/beaumanvienna/JarvisAgent/blob/main/doc/JC_Workflow_Specification.md) and runtime rules
  - Start workflows
  - Monitor workflow execution
  - Export workflows as `.jcwf`
- Enable **n8n → JarvisAgent integration**, where n8n can start workflows and receive completion callbacks.

### Non-Goals
- Replacing the JarvisAgent runtime or execution model
- Client-side workflow execution
- Full bidirectional lossless conversion between n8n and JCWF (initially one-way)

---

## 2. High-Level Architecture

```
+-----------------------------+
| React + React Flow (UI)     |
|  - Workflow Editor          |
|  - Run Monitor              |
|  - Template Browser         |
+-------------+---------------+
              |
      REST + WebSocket
              |
+-------------v---------------+
| JarvisAgent Web Server      |
| (Crow)                      |
|  - Workflow CRUD API        |
|  - Validation API           |
|  - Run Control API          |
|  - WebSocket Broadcasts     |
+-------------+---------------+
              |
+-------------v---------------+
| JarvisAgent Runtime         |
|  - WorkflowRegistry         |
|  - WorkflowRuntimeManager   |
|  - Trigger Engine           |
+-----------------------------+
```

---

## 3. Frontend Stack

### Technology
- **React**
- **TypeScript**
- **Vite**
- **React Flow**
- Optional layout engines:
  - Dagre
  - ELK

### Integration with JarvisAgent
- The React app is built into static assets (`dist/`)
- Crow serves the assets instead of (or alongside) the current static HTML UI
- Existing dashboard functionality (chat, status) is migrated into React routes

---

## 4. Workflow Editor Design (React Flow)

### Node Representation
A React Flow node can correspond with a JCWF task:
- `taskId`
- `type` (`ai_call`, `shell`, `python`, `internal`, etc.)
- `label`
- `doc`
- `working_directory`
- `params`

### Edge Representation
- Directed edges represent `depends_on` relationships
- Graph must remain a **DAG**

### Editor Capabilities
- Drag & drop nodes
- Connect dependencies
- Pan / zoom / minimap
- Auto-layout (DAG)
- Inline validation markers
- Properties panel per node
- Comment / note nodes (editor-only metadata)

### Graph ⇄ JCWF Mapping
- **Graph → JCWF**: compiler generates valid `.jcwf`
- **JCWF → Graph**: importer recreates graph and auto-layouts nodes

---

## 5. Backend API (Crow)

### Workflow Management
- `GET /api/workflows`
- `GET /api/workflows/{id}`
- `POST /api/workflows`
- `PUT /api/workflows/{id}`
- `POST /api/workflows/import`
- `POST /api/workflows/{id}/validate`

### Execution
- `POST /api/workflows/{id}/start`
- `GET /api/workflows/{id}/runs`
- `GET /api/runs/{runId}`
- `GET /api/runs/{runId}/tasks`
- `POST /api/runs/{runId}/cancel` (optional)

### WebSocket Events
- `workflow_list_update`
- `workflow_run_update`
- `task_instance_update`
- `run_log_append`

---

## 6. Validation Strategy

### Tier 1: JCWF Structural Validation
- Required fields (`version`, `id`, `tasks`)
- Valid DAG
- Valid trigger definitions
- Task dependency existence

### Tier 2: JarvisAgent Runtime Rules
- Unique `working_directory` per task
- Only AI tasks may write to queue
- No runtime CWD changes
- Absolute path resolution guarantees
- Missing input sources are hard errors

**Validation is authoritative in C++**, UI displays results.

---

## 7. Templates

Provide built-in templates:
- AI report pipelines
- Document processing pipelines
- File-watcher workflows
- Manual / CLI-triggered workflows

Templates are stored as `.jcwf` and cloned into user workflows.

---

## 8. Run Monitoring UI

### Features
- Workflow run list
- Live DAG with node status overlays
- Per-task inspection:
  - Inputs
  - Outputs
  - Logs
  - Errors

### UX Goal
Match n8n-style observability while preserving JarvisAgent semantics.

---

## 9. n8n Integration

### Integration Model
**HTTP + Webhook callback**

#### JarvisAgent Endpoint
`POST /api/integrations/n8n/start`
- Input:
  - `workflowId`
  - `context`
  - `callbackUrl`
- Output:
  - `runId`

#### Completion Callback
JarvisAgent POSTs to `callbackUrl`:
```json
{
  "runId": "...",
  "status": "success | failed",
  "startedAt": "...",
  "finishedAt": "...",
  "outputs": {},
  "error": null
}
```

### n8n Side
- HTTP Request node → start workflow
- Webhook trigger → receive completion

### Security
- API key or HMAC signature
- Optional callback domain allowlist
- Rate limiting

---

## 10. n8n-Like Features Worth Adding

- Task/node catalog
- Credentials & secrets store
- Sub-workflows (workflow-call node)
- Partial execution (run from node)
- Workflow versioning

---

## 11. Converter Plan

### n8n → JCWF (Initial)
- Support DAG workflows
- Map:
  - HTTP Request → shell/internal task
  - Code → python task (restricted)
  - Webhook → JC trigger
- Generate valid JCWF with runtime-compliant paths

### JCWF → n8n (Optional, later)
- Best-effort export only

---

## 12. Phased Implementation Plan

### Phase 1 – Backend APIs
- Workflow CRUD   (Create, Read, Update, Delete)
- Run control
- Validation endpoint
- WebSocket events

### Phase 2 – React App Scaffold
- Vite + React
- Crow static serving
- Migrate existing dashboard views

### Phase 3 – React Flow Editor
- Graph editing
- Import/export
- Validation UI

### Phase 4 – Run Visualization
- Live DAG status
- Task inspection
- Logs

### Phase 5 – n8n Integration
- Start endpoint
- Callback support
- n8n example workflow

### Phase 6 – Templates & Converter
- Template gallery
- n8n → JCWF converter

---

## 13. Guiding Principles

- JCWF remains the **single source of truth**
- No guessing, no hidden runtime behavior
- Validation errors are explicit and surfaced early
- Editor convenience must never violate [ JC Workflow File Format™ (JCWF)](https://github.com/beaumanvienna/JarvisAgent/blob/main/doc/JC_Workflow_Specification.md) guarantees

---

**End of document**
