# Web Server

REST API + WebSocket server built on the [Crow](https://crowcpp.org/) framework. Serves the React dashboard and workflow editor UIs as static files.

## Key Files

| File | Purpose |
|------|---------|
| `webServer.h/cpp` | Route definitions, request handlers, WebSocket management |
| `chatMessages.h/cpp` | AI assistant chat message types and serialization |

## Endpoints

See `doc/api-endpoints.md` for the full REST API reference.

### Key endpoint groups

- `/api/status` — Server status, connection health, MCP heartbeat
- `/api/workflows` — Workflow listing, CRUD (Studio), run management
- `/api/workflow-runs` — Run status, task states, cancel/pause
- `/api/connections` — Cloud connection CRUD, test, OAuth flow
- `/api/providers` — AI key management
- `/api/webhook` — Inbound webhook triggers
- `/ws` — WebSocket for real-time run/session/log updates

## Editions

Routes are conditionally compiled:
- **Studio** (`J9T_STUDIO`): full CRUD, AI assistant, JCWF generation, connections management, OAuth flow
- **Engine**: read-only workflow listing, run management, webhook triggers, RBAC-enforced

## WebSocket Protocol

The server broadcasts JSON messages to connected clients:
- `workflowRunsSnapshot` — periodic run state updates
- `sessionStatus` — session manager state changes
- `logLine` — real-time log streaming

The upgrade is auth-gated identically to the REST API (`onaccept` runs `Authenticate(req)`); a missing or invalid credential returns a connection refusal that fires the client's `onclose` handler.  The client (dashboard `useWebSocket` hook) reconnects with **exponential backoff** — base 2 s, doubles per consecutive failed connect, capped at 30 s, resets to base on successful `onopen`.  Without the backoff a long-open dashboard tab opened before the user logged in would generate one `[security] auth_failure reason=missing_credential` + one `[security] ws_upgrade_rejected` line every 2 s in `log/security.txt`, drowning genuine signal.
