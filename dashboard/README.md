# Dashboard UI

React frontend for monitoring j9t workflow execution, session status, and logs.

## Quick Start

```bash
npm install
npm run build    # production build -> dist/
npm run dev      # development server with HMR
```

The built `dist/` folder is served by j9t at `http://localhost:8080/`.

## Features

- **Workflow panel** — live run status, task states, start/cancel runs
- **Session panel** — queue-based session monitoring (inflight, completed, failed)
- **Log viewer** — real-time log streaming with search and filtering
- **Status bar** — connection LED, MCP status, cloud health, Python status, run counters

## Architecture

- **Polling** — `/api/status` polled every 5 seconds for server state
- **WebSocket** — real-time updates for run snapshots, session status, log lines
- **No routing** — single-page with tab switching (Dashboard / Log)

## Key Files

| File | Purpose |
|------|---------|
| `src/App.tsx` | Main app, polling, WebSocket, tab management |
| `src/components/StatusBar.tsx` | Top status bar with LED indicators |
| `src/components/WorkflowsPanel.tsx` | Workflow list, run actions, task state display |
| `src/components/SessionManagersPanel.tsx` | Session status cards |
| `src/components/LogViewerPanel.tsx` | Log viewer with byte-offset pagination |
| `src/types.ts` | TypeScript interfaces for API responses |
| `src/hooks/useWebSocket.ts` | WebSocket connection and message parsing |
