// MCP tool implementations — each tool proxies to a j9t REST API endpoint.

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import type { J9tClient } from "./j9tClient.js";

// -- Response types from j9t --

interface WorkflowListItem
{
    id: string;
    label: string;
    manual_start: boolean;
    is_sub_workflow: boolean;
}

interface WorkflowsResponse
{
    ok: boolean;
    workflows: WorkflowListItem[];
}

interface RunStartResponse
{
    ok: boolean;
    enqueued: boolean;
    id: string;
    runId: string;
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

interface ActiveRunsResponse
{
    ok: boolean;
    runs: Array<{
        runId: string;
        workflowId: string;
        state: string;
        startedAt: string;
        completedAt: string;
        taskCount: number;
    }>;
}

interface CancelResponse
{
    ok: boolean;
    cancelRequested: boolean;
    runId: string;
}

export function registerTools(server: McpServer, client: J9tClient): void
{
    // ---- list_workflows ----
    server.tool(
        "list_workflows",
        "List all available workflows in j9t with their labels and whether they support manual start",
        async () =>
        {
            const data = await client.get<WorkflowsResponse>("/api/workflows");
            const lines = data.workflows.map((w) =>
            {
                const flags = [
                    w.manual_start ? "manual_start" : "trigger_only",
                    w.is_sub_workflow ? "sub_workflow" : "top_level",
                ].join(", ");
                return `- **${w.id}** — ${w.label} (${flags})`;
            });

            return {
                content: [{
                    type: "text" as const,
                    text: `Found ${data.workflows.length} workflow(s):\n\n${lines.join("\n")}`,
                }],
            };
        },
    );

    // ---- run_workflow ----
    server.tool(
        "run_workflow",
        "Start a workflow run in j9t. Optionally provide context key-value pairs that tasks can reference.",
        {
            workflowId: z.string().describe("The workflow ID to run"),
            context: z.record(z.string(), z.string()).optional().describe("Key-value context pairs seeded into the run"),
        },
        async ({ workflowId, context }) =>
        {
            const body = context ? { context } : {};
            const data = await client.post<RunStartResponse>(
                `/api/workflows/${encodeURIComponent(workflowId)}/run`,
                body,
            );

            return {
                content: [{
                    type: "text" as const,
                    text: `Workflow **${data.id}** started.\nRun ID: \`${data.runId}\`\n\nUse \`get_run_status\` to monitor progress.`,
                }],
            };
        },
    );

    // ---- get_run_status ----
    server.tool(
        "get_run_status",
        "Get the current status of a workflow run including per-task progress",
        {
            runId: z.string().describe("The run ID returned by run_workflow"),
        },
        async ({ runId }) =>
        {
            const data = await client.get<RunDetailResponse>(
                `/api/workflow-runs/${encodeURIComponent(runId)}`,
            );
            const run = data.run;

            const taskSummary = run.tasks.map((t) =>
                `  - ${t.taskId}: ${t.state}${t.attemptCount > 1 ? ` (${t.attemptCount} attempts)` : ""}`
            );

            const succeeded = run.tasks.filter((t) => t.state === "succeeded").length;
            const failed = run.tasks.filter((t) => t.state === "failed").length;
            const running = run.tasks.filter((t) => t.state === "running").length;
            const pending = run.tasks.filter((t) => t.state === "pending" || t.state === "ready").length;

            return {
                content: [{
                    type: "text" as const,
                    text: [
                        `**Run:** \`${run.runId}\``,
                        `**Workflow:** ${run.workflowId}`,
                        `**State:** ${run.state}`,
                        `**Started:** ${run.startedAt}`,
                        run.completedAt ? `**Completed:** ${run.completedAt}` : "",
                        `**Tasks:** ${succeeded} succeeded, ${failed} failed, ${running} running, ${pending} pending`,
                        "",
                        "**Task details:**",
                        ...taskSummary,
                    ].filter(Boolean).join("\n"),
                }],
            };
        },
    );

    // ---- get_run_output ----
    server.tool(
        "get_run_output",
        "Retrieve the completed output of a workflow run (same data as get_run_status, intended for completed runs)",
        {
            runId: z.string().describe("The run ID to retrieve output for"),
        },
        async ({ runId }) =>
        {
            const data = await client.get<RunDetailResponse>(
                `/api/workflow-runs/${encodeURIComponent(runId)}`,
            );
            const run = data.run;

            if (run.state !== "succeeded" && run.state !== "failed" && run.state !== "cancelled")
            {
                return {
                    content: [{
                        type: "text" as const,
                        text: `Run \`${run.runId}\` is still **${run.state}**. Wait for it to complete before fetching output.`,
                    }],
                };
            }

            const taskDetails = run.tasks.map((t) =>
                `- **${t.taskId}**: ${t.state}${t.attemptCount > 1 ? ` (${t.attemptCount} attempts)` : ""}`
            );

            return {
                content: [{
                    type: "text" as const,
                    text: [
                        `**Run:** \`${run.runId}\` — **${run.state}**`,
                        `**Workflow:** ${run.workflowId}`,
                        `**Duration:** ${run.startedAt} → ${run.completedAt}`,
                        "",
                        "**Tasks:**",
                        ...taskDetails,
                    ].join("\n"),
                }],
            };
        },
    );

    // ---- list_active_runs ----
    server.tool(
        "list_active_runs",
        "List all currently running or queued workflow runs",
        async () =>
        {
            const data = await client.get<ActiveRunsResponse>("/api/workflow-runs/active");

            if (data.runs.length === 0)
            {
                return {
                    content: [{
                        type: "text" as const,
                        text: "No active workflow runs.",
                    }],
                };
            }

            const lines = data.runs.map((r) =>
                `- \`${r.runId}\` — **${r.workflowId}** (${r.state}, ${r.taskCount} tasks, started ${r.startedAt})`
            );

            return {
                content: [{
                    type: "text" as const,
                    text: `**${data.runs.length} active run(s):**\n\n${lines.join("\n")}`,
                }],
            };
        },
    );

    // ---- cancel_run ----
    server.tool(
        "cancel_run",
        "Cancel a running workflow. In-flight tasks may be interrupted.",
        {
            runId: z.string().describe("The run ID to cancel"),
        },
        async ({ runId }) =>
        {
            const data = await client.post<CancelResponse>(
                `/api/workflow-runs/${encodeURIComponent(runId)}/cancel`,
            );

            return {
                content: [{
                    type: "text" as const,
                    text: data.cancelRequested
                        ? `Cancellation requested for run \`${data.runId}\`.`
                        : `Cancel request for \`${runId}\` was not accepted.`,
                }],
            };
        },
    );
}
