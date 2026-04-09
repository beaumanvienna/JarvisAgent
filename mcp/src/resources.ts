// MCP resource implementations — expose j9t data as readable resources.

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import type { J9tClient } from "./j9tClient.js";

interface WorkflowListItem
{
    id: string;
    label: string;
    manual_start: boolean;
    is_sub_workflow: boolean;
    parent_workflow_id?: string;
}

interface WorkflowsResponse
{
    ok: boolean;
    workflows: WorkflowListItem[];
}

interface RunDetailResponse
{
    ok: boolean;
    run: {
        runId: string;
        workflowId: string;
        state: string;
        startedAt: string;
        completedAt: string;
        tasks: Array<{
            taskId: string;
            state: string;
            attemptCount: number;
            startedAt: string;
            completedAt: string;
        }>;
    };
}

export function registerResources(server: McpServer, client: J9tClient): void
{
    // ---- workflow://{id} ----
    server.resource(
        "workflow",
        "workflow://list",
        { description: "List all available workflows" },
        async (uri: URL) =>
        {
            const data = await client.get<WorkflowsResponse>("/api/workflows");

            const summary = data.workflows.map((w) => ({
                id: w.id,
                label: w.label,
                manual_start: w.manual_start,
                is_sub_workflow: w.is_sub_workflow,
                uri: `workflow://${w.id}`,
            }));

            return {
                contents: [{
                    uri: uri.href,
                    mimeType: "application/json",
                    text: JSON.stringify(summary, null, 2),
                }],
            };
        },
    );

    // ---- run://{runId} — template resource ----
    // Individual workflow and run resources are accessed via the tools
    // (get_run_status, get_run_output) since MCP resource templates with
    // dynamic IDs require ResourceTemplate objects. For simplicity in Phase 1,
    // we expose the list resource and rely on tools for individual lookups.
}
