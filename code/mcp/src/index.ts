#!/usr/bin/env node

// j9t MCP Server — exposes JarvisAgent workflows to MCP clients
// (Claude Desktop, Claude Code, and other MCP-compatible tools).
//
// Transport: stdio (default) or SSE (when J9T_MCP_TRANSPORT=sse).
// Auth: Bearer token passthrough to j9t REST API.
//
// Environment variables:
//   J9T_URL          — j9t base URL (default: http://localhost:8080)
//   J9T_TOKEN        — Bearer token for j9t Engine auth
//   J9T_TOKEN_FILE   — Path to file containing the Bearer token
//   J9T_MCP_TRANSPORT — "stdio" (default) or "sse"
//   J9T_MCP_PORT     — SSE port (default: 3100)

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { loadConfig } from "./config.js";
import { J9tClient } from "./j9tClient.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";

/** Send periodic heartbeats so the j9t dashboard can show MCP status. */
function startHeartbeat(client: J9tClient): void
{
    const INTERVAL_MS = 15_000; // 15 seconds

    const beat = async () =>
    {
        try
        {
            await client.post("/api/mcp/heartbeat");
        }
        catch
        {
            // j9t not reachable — will retry next interval
        }
    };

    // Fire immediately, then every INTERVAL_MS
    beat();
    setInterval(beat, INTERVAL_MS);
}

async function main(): Promise<void>
{
    const config = loadConfig();
    const client = new J9tClient(config);

    const server = new McpServer({
        name: "j9t",
        version: "0.1.0",
    });

    // Register all tools and resources
    registerTools(server, client);
    registerResources(server, client);

    // Start heartbeat to j9t so the dashboard can show MCP status
    startHeartbeat(client);

    // Select transport
    const transportType = process.env.J9T_MCP_TRANSPORT ?? "stdio";

    if (transportType === "stdio")
    {
        const transport = new StdioServerTransport();
        await server.connect(transport);
        console.error("j9t MCP server running on stdio");
        console.error(`  j9t URL: ${config.baseUrl}`);
        console.error(`  auth: ${config.token ? "Bearer token configured" : "no token (Studio mode)"}`);
    }
    else if (transportType === "sse")
    {
        // SSE transport requires @modelcontextprotocol/sdk/server/sse
        // Dynamic import to avoid pulling it in for stdio-only deployments
        const { SSEServerTransport } = await import("@modelcontextprotocol/sdk/server/sse.js");
        const port = parseInt(process.env.J9T_MCP_PORT ?? "3100", 10);

        // SSE transport needs an HTTP server — use Node's built-in
        const { createServer } = await import("node:http");

        let sseTransport: InstanceType<typeof SSEServerTransport> | null = null;

        const httpServer = createServer(async (req, res) =>
        {
            if (req.url === "/sse")
            {
                sseTransport = new SSEServerTransport("/messages", res);
                await server.connect(sseTransport);
            }
            else if (req.url === "/messages" && req.method === "POST")
            {
                if (sseTransport)
                {
                    await sseTransport.handlePostMessage(req, res);
                }
                else
                {
                    res.writeHead(503);
                    res.end("No active SSE connection");
                }
            }
            else
            {
                res.writeHead(404);
                res.end("Not found");
            }
        });

        httpServer.listen(port, () =>
        {
            console.error(`j9t MCP server running on SSE — http://localhost:${port}/sse`);
            console.error(`  j9t URL: ${config.baseUrl}`);
            console.error(`  auth: ${config.token ? "Bearer token configured" : "no token (Studio mode)"}`);
        });
    }
    else
    {
        console.error(`Unknown transport: ${transportType}. Use "stdio" or "sse".`);
        process.exit(1);
    }
}

main().catch((err) =>
{
    console.error("Fatal error:", err);
    process.exit(1);
});
