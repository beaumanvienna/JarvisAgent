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
    | REST API (localhost, Bearer token auth)
    |
j9t Engine / Studio
```

The MCP server is a thin proxy — it translates MCP tool calls into j9t REST API requests and returns the results.

## Tools

| Tool | j9t API | Description |
|------|---------|-------------|
| `list_workflows` | `GET /api/workflows` | List available workflows with labels |
| `run_workflow` | `POST /api/workflows/<id>/run` | Start a workflow with optional context/bindings |
| `get_run_status` | `GET /api/workflow-runs/<runId>` | Task-level progress and state |
| `get_run_output` | `GET /api/workflow-runs/<runId>` | Retrieve completed workflow results |
| `list_active_runs` | `GET /api/workflow-runs/active` | Currently running workflows |
| `cancel_run` | `POST /api/workflow-runs/<runId>/cancel` | Cancel a running workflow |

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

# Run (connects to j9t on localhost:8080 by default)
npm start

# Or run in dev mode (auto-recompile)
npm run dev
```

## Configuration

Configuration is via environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `J9T_URL` | `http://localhost:8080` | j9t REST API base URL |
| `J9T_TOKEN` | *(empty)* | Bearer token for j9t Engine auth |
| `J9T_TOKEN_FILE` | *(none)* | Path to a file containing the Bearer token |

For TLS-enabled j9t instances, use `https://localhost:8443` as `J9T_URL`.

If neither `J9T_TOKEN` nor `J9T_TOKEN_FILE` is set, requests are sent without authentication (works with j9t Studio on localhost).

## Claude Desktop Integration

Add to your Claude Desktop config (`~/.config/claude/claude_desktop_config.json` on Linux, `~/Library/Application Support/Claude/claude_desktop_config.json` on macOS):

```json
{
  "mcpServers": {
    "j9t": {
      "command": "node",
      "args": ["/path/to/jarvisAgent/mcp/dist/index.js"],
      "env": {
        "J9T_URL": "https://localhost:8443"
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
        "J9T_URL": "https://localhost:8443"
      }
    }
  }
}
```

## Docker Deployment

The MCP server can run as a sidecar container alongside j9t:

```yaml
# docker-compose.yml
mcp:
  build: ./mcp
  depends_on: [jarvisagent]
  environment:
    J9T_URL: http://jarvisagent:8080
    J9T_TOKEN_FILE: /app/engine_api_token.txt
  volumes:
    - ./data:/app:ro
```

Build the image:

```bash
cd mcp
npm run build
docker build -t j9t-mcp .
```

## Production Notes

- The MCP sidecar targets **j9t Engine** for production deployment (security-hardened, RBAC-enforced)
- It also works with **j9t Studio** during development and testing
- RBAC: MCP tools respect j9t roles — viewers can list/get, operators can run/cancel
- Bearer token auth is required when connecting to j9t Engine

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
