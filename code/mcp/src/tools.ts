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

    // ---- debug_signals ----
    // Always registered on the sidecar; the server only responds in debug builds
    // (release builds compile the route out entirely and return 404). Handle that
    // gracefully so the LLM sees a clear "not available" instead of a crash.
    server.tool(
        "debug_signals",
        "Live j9t engine introspection — AI inflight, python pool state, workflow runs, adhoc state, websocket clients, uptime, and more. Debug builds only; release builds return a 'not available' message.",
        async () =>
        {
            try
            {
                const data = await client.get<{ ok: boolean; signals?: Record<string, unknown> }>(
                    "/api/debug/signals",
                );
                if (!data.ok || !data.signals)
                {
                    return {
                        content: [{ type: "text" as const, text: "debug_signals returned no data." }],
                    };
                }
                return {
                    content: [{
                        type: "text" as const,
                        text: "```json\n" + JSON.stringify(data.signals, null, 2) + "\n```",
                    }],
                };
            }
            catch (err)
            {
                const msg = err instanceof Error ? err.message : String(err);
                const notAvailable = msg.includes("404");
                return {
                    content: [{
                        type: "text" as const,
                        text: notAvailable
                            ? "debug_signals is not available — this j9t instance was built without the DEBUG flag. Rebuild with `make config=debug` to enable the endpoint."
                            : `debug_signals call failed: ${msg}`,
                    }],
                };
            }
        },
    );

    // ---- whoami ----
    server.tool(
        "whoami",
        "Return the identity and role (admin | operator | viewer) associated with the current MCP key",
        async () =>
        {
            const data = await client.get<{ ok: boolean; user: string; role: string; error?: string }>(
                "/api/auth/whoami",
            );
            if (!data.ok)
            {
                return {
                    content: [{ type: "text" as const, text: `Not authenticated: ${data.error ?? "unknown"}` }],
                };
            }
            return {
                content: [{
                    type: "text" as const,
                    text: `**User:** ${data.user}\n**Role:** ${data.role}`,
                }],
            };
        },
    );

    // ---- get_run_logs ----
    server.tool(
        "get_run_logs",
        "Tail the j9t application log. Returns the last N lines as they appear in log/log.txt.",
        {
            tail: z.number().int().positive().max(5000).optional().default(200)
                .describe("Number of most-recent lines to return (1–5000). Defaults to 200."),
        },
        async ({ tail }) =>
        {
            const data = await client.get<{ ok: boolean; lines: string[]; totalSize?: number }>(
                `/api/log?tail=${tail}`,
            );
            if (!data.ok)
            {
                return {
                    content: [{ type: "text" as const, text: "Failed to read log — is the server running?" }],
                };
            }
            const text = data.lines.join("\n");
            return {
                content: [{
                    type: "text" as const,
                    text: text || "(log is empty)",
                }],
            };
        },
    );

    // ---- validate_workflow (Studio only) ----
    server.tool(
        "validate_workflow",
        "Validate a JCWF canvas payload without saving it. Requires Studio edition.",
        {
            jcwf: z.record(z.string(), z.unknown())
                .describe("Complete JCWF canvas JSON (same shape as POST /api/workflows body)"),
        },
        async ({ jcwf }) =>
        {
            const data = await client.post<{
                ok: boolean;
                error?: string;
                findings?: Array<{ severity: string; message: string; field?: string }>;
            }>("/api/workflows/validate", jcwf);

            if (!data.ok)
            {
                const findings = (data.findings ?? [])
                    .map((f) => `  - [${f.severity}] ${f.field ? `${f.field}: ` : ""}${f.message}`)
                    .join("\n");
                return {
                    content: [{
                        type: "text" as const,
                        text: `Validation failed: ${data.error ?? "unknown"}\n${findings}`,
                    }],
                };
            }
            return {
                content: [{
                    type: "text" as const,
                    text: `Workflow validated successfully.`,
                }],
            };
        },
    );

    // ---- reload_workflows (Studio only) ----
    server.tool(
        "reload_workflows",
        "Re-scan the workflows folder and reload the WorkflowRegistry. Use after editing JCWF files on disk so j9t picks up the changes without a restart. Requires Studio edition.",
        {},
        async () =>
        {
            const data = await client.post<{ reloaded: boolean; workflowCount?: number; ok: boolean; error?: string }>(
                "/api/workflows/reload",
                {},
            );
            if (!data.ok)
            {
                return {
                    content: [{
                        type: "text" as const,
                        text: `Reload failed: ${data.error ?? "unknown error"}`,
                    }],
                };
            }
            return {
                content: [{
                    type: "text" as const,
                    text: `Workflows reloaded. Registry now holds **${data.workflowCount ?? "?"}** workflow(s).`,
                }],
            };
        },
    );

    // ---- upload_workflow (Studio only) ----
    server.tool(
        "upload_workflow",
        "Register a JCWF as a permanent workflow in j9t. Requires Studio edition. Use run_adhoc_workflow for one-shot execution.",
        {
            jcwf: z.record(z.string(), z.unknown())
                .describe("Complete JCWF canvas JSON to register"),
        },
        async ({ jcwf }) =>
        {
            const data = await client.post<{ ok: boolean; id?: string; error?: string }>(
                "/api/workflows",
                jcwf,
            );
            if (!data.ok)
            {
                return {
                    content: [{
                        type: "text" as const,
                        text: `Upload failed: ${data.error ?? "unknown error"}`,
                    }],
                };
            }
            return {
                content: [{
                    type: "text" as const,
                    text: `Workflow **${data.id}** registered. Use \`run_workflow\` to start it.`,
                }],
            };
        },
    );

    // ---- manage_connections ----
    server.tool(
        "manage_connections",
        "CRUD + connectivity test on cloud connections (Jira, Slack, Snowflake, S3, etc.). Admin role required.",
        {
            action: z.enum(["list", "get", "create", "update", "delete", "test"])
                .describe("Operation to perform"),
            name: z.string().optional()
                .describe("Connection name — required for get/update/delete/test"),
            connection: z.record(z.string(), z.unknown()).optional()
                .describe("Connection payload — required for create/update"),
        },
        async ({ action, name, connection }) =>
        {
            if (action === "list")
            {
                const data = await client.get<{
                    ok: boolean;
                    connections: Array<{ name: string; type: string; endpoint: string; key_name: string; auth_type: string }>;
                }>("/api/connections");
                if (!data.ok || data.connections.length === 0)
                {
                    return { content: [{ type: "text" as const, text: "No connections configured." }] };
                }
                const lines = data.connections.map((c) =>
                    `- **${c.name}** (${c.type}) → ${c.endpoint} [auth: ${c.auth_type}, key: ${c.key_name || "—"}]`
                );
                return { content: [{ type: "text" as const, text: `**${data.connections.length} connection(s):**\n\n${lines.join("\n")}` }] };
            }

            if (!name && (action === "get" || action === "update" || action === "delete" || action === "test"))
            {
                return { content: [{ type: "text" as const, text: `Action \`${action}\` requires a \`name\`.` }] };
            }

            if (action === "get")
            {
                // The list endpoint is the get endpoint — scan results for the requested name.
                const data = await client.get<{
                    ok: boolean;
                    connections: Array<{ name: string }>;
                }>("/api/connections");
                const match = (data.connections ?? []).find((c) => (c as any).name === name);
                if (!match) return { content: [{ type: "text" as const, text: `Connection \`${name}\` not found.` }] };
                return { content: [{ type: "text" as const, text: "```json\n" + JSON.stringify(match, null, 2) + "\n```" }] };
            }

            if (action === "create")
            {
                if (!connection) return { content: [{ type: "text" as const, text: "Action `create` requires a `connection` payload." }] };
                const data = await client.post<{ ok: boolean; error?: string }>("/api/connections", connection);
                return { content: [{ type: "text" as const, text: data.ok ? `Connection created.` : `Create failed: ${data.error ?? "unknown"}` }] };
            }

            if (action === "update")
            {
                if (!connection) return { content: [{ type: "text" as const, text: "Action `update` requires a `connection` payload." }] };
                const data = await client.put<{ ok: boolean; error?: string }>(
                    `/api/connections/${encodeURIComponent(name!)}`,
                    connection,
                );
                return { content: [{ type: "text" as const, text: data.ok ? `Connection \`${name}\` updated.` : `Update failed: ${data.error ?? "unknown"}` }] };
            }

            if (action === "delete")
            {
                const data = await client.delete<{ ok: boolean; error?: string }>(
                    `/api/connections/${encodeURIComponent(name!)}`,
                );
                return { content: [{ type: "text" as const, text: data.ok ? `Connection \`${name}\` deleted.` : `Delete failed: ${data.error ?? "unknown"}` }] };
            }

            // action === "test"
            const data = await client.post<{ ok: boolean; error?: string; message?: string }>(
                `/api/connections/${encodeURIComponent(name!)}/test`,
            );
            return { content: [{ type: "text" as const, text: data.ok ? `Connection \`${name}\` OK: ${data.message ?? "reachable"}` : `Test failed: ${data.error ?? "unreachable"}` }] };
        },
    );

    // ---- run_adhoc_workflow ----
    server.tool(
        "run_adhoc_workflow",
        "Submit a JCWF for one-shot execution. No permanent registration; scripts referenced by the JCWF must already exist under scripts/. Requires an MCP key with adhoc_enabled and role >= operator.",
        {
            jcwf: z.record(z.string(), z.unknown())
                .describe("Complete JCWF canvas JSON"),
            context: z.record(z.string(), z.string()).optional()
                .describe("Key-value context pairs seeded into the run"),
            cleanup_policy: z.enum([
                "on_completion", "ttl_1h", "ttl_24h", "ttl_48h", "ttl_72h", "retain"
            ]).optional()
                .describe("When to delete the run's folder. Defaults to the MCP key's configured default. Cannot exceed the user's ceiling."),
        },
        async ({ jcwf, context, cleanup_policy }) =>
        {
            const body: Record<string, unknown> = { jcwf };
            if (context) body.context = context;
            if (cleanup_policy) body.cleanup_policy = cleanup_policy;

            const data = await client.post<{
                ok: boolean;
                runId?: string;
                workflowId?: string;
                cleanup_policy?: string;
                error?: string;
                message?: string;
            }>("/api/workflows/run-adhoc", body);

            if (!data.ok)
            {
                return {
                    content: [{
                        type: "text" as const,
                        text: `Adhoc submission failed: ${data.error ?? "unknown"}${data.message ? ` — ${data.message}` : ""}`,
                    }],
                };
            }

            return {
                content: [{
                    type: "text" as const,
                    text: [
                        `Adhoc workflow **${data.workflowId}** staged.`,
                        `Run ID: \`${data.runId}\``,
                        `Cleanup policy: \`${data.cleanup_policy}\``,
                        ``,
                        `Monitor with \`get_run_status\`.`,
                    ].join("\n"),
                }],
            };
        },
    );

    // ---- list_scripts ----
    server.tool(
        "list_scripts",
        "List the scripts pre-deployed under scripts/ that adhoc JCWFs can reference. Returns each script's metadata (short description, params, outputs, type) parsed from its @jarvis-script header. Use this to discover what's available before composing a run_adhoc_workflow payload — submitting a JCWF that references a non-existent script is rejected with 400 missing_scripts.",
        {
            type: z.enum(["shell", "python"]).optional()
                .describe("Filter by script type (shell or python). Omit to list everything."),
            refresh: z.boolean().optional()
                .describe("Re-scan scripts/ before returning (use when the admin has just deployed new scripts)."),
        },
        async ({ type, refresh }) =>
        {
            const qs = new URLSearchParams();
            if (type) qs.set("type", type);
            if (refresh) qs.set("refresh", "1");
            const query = qs.toString() ? `?${qs.toString()}` : "";
            const data = await client.get<{
                ok: boolean;
                count?: number;
                scripts?: Array<{
                    path: string;
                    type: string;
                    module?: string;
                    short?: string;
                    description?: string;
                    outputs?: string;
                    params?: string[];
                    has_jarvis_marker?: boolean;
                    executable?: boolean;
                }>;
                error?: string;
                message?: string;
            }>(`/api/scripts${query}`);

            if (!data.ok)
            {
                return {
                    content: [{
                        type: "text" as const,
                        text: `list_scripts failed: ${data.error ?? "unknown"}${data.message ? ` — ${data.message}` : ""}`,
                    }],
                };
            }

            const scripts = data.scripts ?? [];
            if (scripts.length === 0)
            {
                return {
                    content: [{
                        type: "text" as const,
                        text: `No scripts found${type ? ` for type '${type}'` : ""}.`,
                    }],
                };
            }

            const lines: string[] = [`**Scripts (${scripts.length}):**`, ``];
            for (const s of scripts)
            {
                const flags: string[] = [];
                if (s.has_jarvis_marker === false) flags.push("no @jarvis-script");
                if (s.executable === false && s.type === "shell") flags.push("not executable");
                const flagSuffix = flags.length ? ` _(${flags.join(", ")})_` : "";
                const ident = s.type === "python" && s.module ? `\`${s.module}\` (${s.path})` : `\`${s.path}\``;
                lines.push(`- **${ident}** — ${s.type}${flagSuffix}`);
                if (s.short) lines.push(`    ${s.short}`);
                if (s.params && s.params.length > 0)
                {
                    lines.push(`    params: \`${s.params.join(" ")}\``);
                }
                if (s.outputs) lines.push(`    outputs: ${s.outputs}`);
                if (s.description) lines.push(`    _${s.description}_`);
            }

            return {
                content: [{ type: "text" as const, text: lines.join("\n") }],
            };
        },
    );

    // ---- list_run_files ----
    server.tool(
        "list_run_files",
        "Discover the files a workflow run produced. Returns each file's path, size, mtime, content-type, task attribution, plus both a local filesystem path (for agents on the same host as j9t) and a download URL (for remote agents). Also reports retention policy so the caller knows how long the artefacts will live.",
        {
            runId: z.string().describe("The run ID returned by run_workflow or run_adhoc_workflow"),
            prefix: z.string().optional()
                .describe("Optional path prefix filter (e.g. 'queue/myFlow/parse/'). Lexically normalised; '..' rejected."),
        },
        async ({ runId, prefix }) =>
        {
            const qs = new URLSearchParams();
            if (prefix) qs.set("prefix", prefix);
            const query = qs.toString() ? `?${qs.toString()}` : "";
            const data = await client.get<{
                ok: boolean;
                runId?: string;
                owner?: string;
                terminal?: boolean;
                retention?: { policy?: string; delete_at?: string; seconds_remaining?: number };
                files?: Array<{
                    path: string;
                    size_bytes: number;
                    modified_at?: string;
                    task_id?: string;
                    content_type?: string;
                    local_path?: string;
                    download_url?: string;
                }>;
                error?: string;
                message?: string;
            }>(
                `/api/workflow-runs/${encodeURIComponent(runId)}/files${query}`,
            );

            if (!data.ok)
            {
                return {
                    content: [{
                        type: "text" as const,
                        text: `list_run_files failed: ${data.error ?? "unknown"}${data.message ? ` — ${data.message}` : ""}`,
                    }],
                };
            }

            const files = data.files ?? [];
            if (files.length === 0)
            {
                return {
                    content: [{
                        type: "text" as const,
                        text: `Run ${data.runId} (owner: ${data.owner ?? "unknown"}) produced no files${prefix ? ` matching prefix '${prefix}'` : ""}.`,
                    }],
                };
            }

            const retention = data.retention ?? {};
            const retLine = [
                retention.policy ? `policy=${retention.policy}` : "",
                retention.delete_at ? `delete_at=${retention.delete_at}` : "",
                retention.seconds_remaining !== undefined ? `seconds_remaining=${retention.seconds_remaining}` : "",
            ].filter(Boolean).join(", ");

            const lines: string[] = [
                `**Run:** \`${data.runId}\` (owner: ${data.owner ?? "unknown"}, terminal: ${data.terminal ? "yes" : "no"})`,
                `**Retention:** ${retLine || "unknown"}`,
                ``,
                `**Files (${files.length}):**`,
            ];
            for (const f of files)
            {
                const taskHint = f.task_id ? ` [task=${f.task_id}]` : "";
                lines.push(`- \`${f.path}\` — ${f.size_bytes} bytes, ${f.content_type ?? "?"}${taskHint}`);
                if (f.local_path) lines.push(`    local: \`${f.local_path}\``);
                if (f.download_url) lines.push(`    download: \`${f.download_url}\``);
            }

            return {
                content: [{
                    type: "text" as const,
                    text: lines.join("\n"),
                }],
            };
        },
    );

    // ---- get_run_file ----
    server.tool(
        "get_run_file",
        "Download a single artefact file from a workflow run. Honours the caller's MCP identity (operator reads own runs; admin reads any). Text content is returned inline; binary content is returned base64-encoded. Range is supported for large files — the server caps a single response at 10 MB.",
        {
            runId: z.string().describe("The run ID returned by run_workflow or run_adhoc_workflow"),
            path: z.string().describe("Path relative to the run folder (get from list_run_files)"),
            range_start: z.number().optional().describe("Inclusive byte offset. Use with range_end to fetch a slice of a large file."),
            range_end: z.number().optional().describe("Inclusive byte offset. Use with range_start."),
        },
        async ({ runId, path, range_start, range_end }) =>
        {
            const headers: Record<string, string> = {};
            if (range_start !== undefined || range_end !== undefined)
            {
                const s = range_start ?? 0;
                const e = range_end ?? "";
                headers["Range"] = `bytes=${s}-${e}`;
            }

            const response = await client.fetchRaw(
                `/api/workflow-runs/${encodeURIComponent(runId)}/files/${path.split("/").map(encodeURIComponent).join("/")}`,
                { headers },
            );

            if (!response.ok && response.status !== 206)
            {
                // Try to surface a structured error if the body is JSON.
                let errBody: { error?: string; message?: string } = {};
                try { errBody = await response.json(); } catch { /* binary / non-json */ }
                return {
                    content: [{
                        type: "text" as const,
                        text: `get_run_file failed: ${response.status}${errBody.error ? ` ${errBody.error}` : ""}${errBody.message ? ` — ${errBody.message}` : ""}`,
                    }],
                };
            }

            const contentType = response.headers.get("content-type") ?? "application/octet-stream";
            const sha256 = response.headers.get("x-content-sha256") ?? "";
            const contentRange = response.headers.get("content-range") ?? "";
            const deleteAt = response.headers.get("x-retention-delete-at") ?? "";

            // Text content-types return inline; binary returns base64 so an agent
            // can round-trip it through stdout.
            const isText = contentType.startsWith("text/") ||
                           contentType === "application/json" ||
                           contentType === "application/xml" ||
                           contentType === "application/yaml";

            const header = [
                `**File:** \`${path}\` (${response.status}${contentRange ? `, ${contentRange}` : ""})`,
                `**Content-Type:** ${contentType}`,
                sha256 ? `**SHA-256:** \`${sha256}\`` : "",
                deleteAt ? `**Retention delete-at:** ${deleteAt}` : "",
                ``,
            ].filter(Boolean).join("\n");

            if (isText)
            {
                const text = await response.text();
                return {
                    content: [{ type: "text" as const, text: header + text }],
                };
            }

            const buf = new Uint8Array(await response.arrayBuffer());
            const base64 = Buffer.from(buf).toString("base64");
            return {
                content: [{
                    type: "text" as const,
                    text: header + `\`\`\`base64\n${base64}\n\`\`\``,
                }],
            };
        },
    );

    // ---- manage_keys ----
    server.tool(
        "manage_keys",
        "CRUD on AI provider credentials (OpenAI, Anthropic, Gemini, etc.). Admin role required. Requires Studio edition.",
        {
            action: z.enum(["list", "create", "update", "delete", "set_default"])
                .describe("Operation to perform"),
            name: z.string().optional()
                .describe("Provider name — required for update/delete/set_default"),
            provider: z.record(z.string(), z.unknown()).optional()
                .describe("Provider payload — required for create/update"),
        },
        async ({ action, name, provider }) =>
        {
            if (action === "list")
            {
                const data = await client.get<{
                    ok: boolean;
                    default_provider?: string;
                    providers: Array<{ name: string; display_name?: string; endpoint?: string; api_type?: string; has_key?: boolean; credential_type?: string }>;
                }>("/api/settings/providers");
                if (!data.ok || data.providers.length === 0)
                {
                    return { content: [{ type: "text" as const, text: "No providers configured." }] };
                }
                const lines = data.providers.map((p) => {
                    const star = p.name === data.default_provider ? " (default)" : "";
                    const key = p.has_key ? "[key]" : "[no-key]";
                    return `- **${p.name}**${star} — ${p.display_name ?? ""} → ${p.endpoint ?? ""} ${key} (${p.credential_type ?? "api_key"})`;
                });
                return { content: [{ type: "text" as const, text: `**${data.providers.length} provider(s):**\n\n${lines.join("\n")}` }] };
            }

            if ((action === "update" || action === "delete" || action === "set_default") && !name)
            {
                return { content: [{ type: "text" as const, text: `Action \`${action}\` requires a \`name\`.` }] };
            }

            if (action === "create")
            {
                if (!provider) return { content: [{ type: "text" as const, text: "Action `create` requires a `provider` payload." }] };
                const data = await client.post<{ ok: boolean; error?: string }>("/api/settings/providers", provider);
                return { content: [{ type: "text" as const, text: data.ok ? `Provider created.` : `Create failed: ${data.error ?? "unknown"}` }] };
            }

            if (action === "update")
            {
                if (!provider) return { content: [{ type: "text" as const, text: "Action `update` requires a `provider` payload." }] };
                const data = await client.put<{ ok: boolean; error?: string }>(
                    `/api/settings/providers/${encodeURIComponent(name!)}`,
                    provider,
                );
                return { content: [{ type: "text" as const, text: data.ok ? `Provider \`${name}\` updated.` : `Update failed: ${data.error ?? "unknown"}` }] };
            }

            if (action === "delete")
            {
                const data = await client.delete<{ ok: boolean; error?: string }>(
                    `/api/settings/providers/${encodeURIComponent(name!)}`,
                );
                return { content: [{ type: "text" as const, text: data.ok ? `Provider \`${name}\` deleted.` : `Delete failed: ${data.error ?? "unknown"}` }] };
            }

            // action === "set_default"
            const data = await client.post<{ ok: boolean; error?: string }>(
                `/api/settings/providers/${encodeURIComponent(name!)}/default`,
            );
            return { content: [{ type: "text" as const, text: data.ok ? `Default provider set to \`${name}\`.` : `Failed: ${data.error ?? "unknown"}` }] };
        },
    );
}
