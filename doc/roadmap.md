# j9t Roadmap

## 1.0 Release — Feature Freeze Scope

### Adhoc Workflow Submission

Turn j9t into a general-purpose orchestration backend by allowing external callers to submit workflows dynamically via the REST API.

- `POST /api/workflows/run-adhoc` accepts a JCWF payload (zip or inline JSON)
- Stages the workflow in a temporary directory, executes it, returns results
- Enables autonomous AI agents (Claude Code, custom agents, any HTTP client) to compose and submit workflows on the fly
- Cleanup policy for temp workflows (TTL-based or on-completion)

### MCP Configure-Plane Tools

The current MCP interface covers the "run" plane (list/run/status/cancel workflows). Add tools for the "configure" plane so MCP clients are fully self-sufficient without falling back to curl:

- `manage_connections` — CRUD + test connections
- `manage_keys` — create/update credentials
- `upload_workflow` — submit a JCWF (ties into adhoc submission above)
- `get_run_logs` — tail task output without reading local files

Each tool proxies to an existing REST endpoint, so the backend work is minimal.

---

## Post-1.0 — Future Directions

### AI Assistant Generator

An AI assistant framework where the agent's "tools" are JCWFs. The assistant has:

- A predefined set of tool-workflows it can invoke
- Access to project specs and documentation it can read and understand
- The ability to compose new workflows on the fly (via adhoc submission)
- Project folder awareness for context-driven orchestration

j9t becomes the execution layer for an autonomous agent that can reason about goals and orchestrate multi-step workflows to accomplish them. Builds on the JCWF format, AI generation pipeline, and MCP interface already in place.

### Bounded Loops in JCWFs (Evaluate)

Investigate bounded iteration support for use cases like iterative AI refinement ("generate, review, revise" cycles). Key constraints:

- Must preserve termination guarantees (mandatory `max_iterations` + explicit break condition)
- Progress tracking and visualization must remain clear
- Most polling patterns are already covered by the trigger system (email_watch, s3_watch, etc.)
- Retry-until-success is already handled at the executor level

Evaluate real demand before committing — the DAG model's simplicity is a feature, not a limitation.
