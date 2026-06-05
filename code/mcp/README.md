# j9t MCP Server

A standalone [Model Context Protocol](https://modelcontextprotocol.io/) (MCP) server that exposes j9t (JarvisAgent) workflows to Claude Desktop, Claude Code, and other MCP clients.

## Architecture

```
AI Client (Claude Desktop, Claude Code, etc.)
    |
    | MCP protocol (JSON-RPC over stdio)
    |
j9t MCP Server (this package)
    |
    | REST API (localhost, Authorization: Bearer mcp_...)
    |
j9t Engine / Studio
```

The MCP server is a thin proxy — it translates MCP tool calls into j9t REST API requests and returns the results.

## Tools

### Run plane

| Tool | j9t API | Description |
|------|---------|-------------|
| `list_workflows` | `GET /api/workflows` | List available workflows with labels |
| `run_workflow` | `POST /api/workflows/<id>/run` | Start a workflow with optional context/bindings |
| `get_run_status` | `GET /api/workflow-runs/<runId>` | Task-level progress and state |
| `get_run_output` | `GET /api/workflow-runs/<runId>` | Retrieve completed workflow results |
| `list_active_runs` | `GET /api/workflow-runs/active` | Currently running workflows |
| `cancel_run` | `POST /api/workflow-runs/<runId>/cancel` | Cancel a running workflow |
| `run_adhoc_workflow` | `POST /api/workflows/run-adhoc` | Submit a JCWF for one-shot execution (requires `adhoc_enabled`). Supports shell, python, internal, and `ai_call` tasks end-to-end. For `ai_call` tasks set `working_directory` to `"../../queue/<task_id>"` so queue-binding files land in the per-run queue folder (the runtime registers it with the file watcher at stage time). |

### Artifact plane

| Tool | j9t API | Description |
|------|---------|-------------|
| `list_run_files` | `GET /api/workflow-runs/<runId>/files` | Discover files a run produced. Returns path, size, mtime, content-type, task attribution, `local_path` (same-host agents) and `download_url` (remote agents), plus retention so the caller knows how long the artefacts live. Honours ownership: operator reads own runs, admin reads any. |
| `get_run_file` | `GET /api/workflow-runs/<runId>/files/<path>` | Stream a single artefact. Text content is returned inline; binary content is base64-encoded. Range is supported for large files; single response capped at 10 MB server-side. Responses carry `X-Content-SHA256` for integrity verification. |
| `list_scripts` | `GET /api/scripts` | Catalog of scripts under `scripts/` with `@jarvis-script` metadata (short, params, outputs). Use before composing a `run_adhoc_workflow` payload — the adhoc submit rejects JCWFs that reference scripts not on disk. Viewer+ access. |

### Configure plane

| Tool | j9t API | Min role |
|------|---------|----------|
| `manage_connections` | CRUD + test on `/api/connections` | admin |
| `manage_keys` | CRUD + set-default on `/api/settings/providers` (Studio) | admin |
| `upload_workflow` | `POST /api/workflows` (Studio) | admin |
| `validate_workflow` | `POST /api/workflows/validate` (Studio) | operator |

### Observability / auth

| Tool | j9t API |
|------|---------|
| `get_run_logs` | `GET /api/log?tail=N` |
| `whoami` | `GET /api/auth/whoami` |

## Resources

| URI | Description |
|-----|-------------|
| `workflow://{id}` | Workflow definition and metadata |
| `run://{runId}` | Run status, task states, outputs |

## Quick Start

```bash
# Install dependencies
npm install

# Build
npm run build

# Run (connects to https://localhost:8443 by default; point NODE_EXTRA_CA_CERTS
# at j9t's self-signed cert so Node trusts it)
NODE_EXTRA_CA_CERTS=~/JarvisAgent/certs/j9t-cert.pem J9T_TOKEN=mcp_... npm start

# Or run in dev mode (auto-recompile)
J9T_TOKEN=mcp_... npm run dev
```

## Authentication

j9t accepts only MCP API keys (`mcp_` + 64 hex chars) as the bearer credential for programmatic access. The legacy shared admin token has been removed — every machine credential is per-user and individually revocable.

### Getting your first MCP key

On j9t's first run with an empty key store, it prints a bootstrap enrollment token to stderr. Activate it to receive your MCP admin key:

```bash
# Copy the enroll_... token from j9t's stderr output, then (-k: self-signed cert):
curl -sSk -X POST https://localhost:8443/api/auth/mcp-keys/activate \
     -H 'Content-Type: application/json' \
     -d '{"enrollment_token":"enroll_..."}'

# Response:
# { "ok": true, "key_id": "mcp_a1b2c3d4",
#   "api_key": "mcp_a1b2c3d4e5f6...",  <-- save this; shown exactly once
#   ... }
```

For subsequent users, an admin creates an enrollment via the Settings > MCP Keys tab or `POST /api/auth/mcp-keys/enroll`, and the user activates it with the same endpoint.

## Configuration

Configuration is via environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `J9T_URL` | `https://localhost:8443` | j9t REST API base URL |
| `J9T_TOKEN` | *(empty)* | MCP API key (must start with `mcp_`) |
| `J9T_TOKEN_FILE` | *(none)* | Path to a file containing the MCP API key |
| `NODE_EXTRA_CA_CERTS` | *(none)* | Path to j9t's self-signed cert (`certs/j9t-cert.pem`) so Node's `fetch` trusts the default HTTPS endpoint |

A default j9t serves HTTPS on 8443 and mints its own self-signed `localhost` cert on first start. Point `NODE_EXTRA_CA_CERTS` at `certs/j9t-cert.pem` in the j9t working directory (e.g. `~/JarvisAgent/certs/j9t-cert.pem`) so the sidecar's TLS verification passes. For a plain-HTTP j9t (e.g. Docker's default on port 8080), set `J9T_URL=http://localhost:8080` explicitly.

If neither `J9T_TOKEN` nor `J9T_TOKEN_FILE` is set, the sidecar connects without auth. j9t **rejects** all authenticated endpoints in that case — the sidecar will only succeed on the public health check. Set a real MCP key for any real work.

## Claude Desktop Integration

Add to your Claude Desktop config (`~/.config/claude/claude_desktop_config.json` on Linux, `~/Library/Application Support/Claude/claude_desktop_config.json` on macOS):

```json
{
  "mcpServers": {
    "j9t": {
      "command": "node",
      "args": ["/path/to/jarvisAgent/mcp/dist/index.js"],
      "env": {
        "J9T_URL": "https://localhost:8443",
        "J9T_TOKEN": "mcp_..."
      }
    }
  }
}
```

## Claude Code Integration

Add to your Claude Code settings (`.claude/settings.json` or project `CLAUDE.md`):

```json
{
  "mcpServers": {
    "j9t": {
      "command": "node",
      "args": ["/path/to/jarvisAgent/mcp/dist/index.js"],
      "env": {
        "J9T_URL": "https://localhost:8443",
        "J9T_TOKEN": "mcp_..."
      }
    }
  }
}
```

## Docker Deployment

The MCP server can run as a sidecar container alongside j9t. Point it at a mounted file containing the MCP key:

```yaml
# docker-compose.yml
mcp:
  build: ./mcp
  depends_on: [jarvisagent]
  environment:
    J9T_URL: http://jarvisagent:8080
    J9T_TOKEN_FILE: /secrets/mcp_key
  volumes:
    - ./secrets:/secrets:ro
```

Build the image:

```bash
cd mcp
npm run build
docker build -t j9t-mcp .
```

## Production Notes

- The MCP sidecar targets **j9t Engine** for production deployment (security-hardened, RBAC-enforced, no workflow CRUD exposed).
- It also works with **j9t Studio** during development and testing — Studio adds `upload_workflow`, `validate_workflow`, and `manage_keys` tools that are unavailable on Engine.
- **RBAC is key-scoped.** Each MCP user gets their own key with a role (admin / operator / viewer) and an `adhoc_enabled` flag set by the admin at enrollment time. Routes enforce the minimum required role; calls from under-privileged keys return HTTP 403.
- **Audit.** Every MCP-authenticated request appears in `log/security.txt` tagged with the user, role, and endpoint.

## Source Files

```
mcp/
  src/
    index.ts       — MCP server entry point, transport setup (stdio)
    tools.ts       — Tool handler implementations
    resources.ts   — Resource handler implementations
    j9tClient.ts   — HTTP client for j9t REST API
    config.ts      — Configuration from environment variables
  Dockerfile       — Production container image
  package.json     — Dependencies (@modelcontextprotocol/sdk)
  tsconfig.json    — TypeScript configuration
```

## License

MIT
