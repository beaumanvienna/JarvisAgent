import type { EditorControlNode, EditorGraph, EditorFilterNode, EditorTaskEdge, EditorTaskNode, EditorNode } from "./types";
import type { JcwfControlNode, JcwfControlflowEdge, JcwfControlflowKind, JcwfDataflowEntry, JcwfFile, JcwfFilter, JcwfTask } from "../jcwf/types";
import { deriveUpstreamOutputPaths, fileNodeInputPath } from "./pathSynthesis";
import { readsFileInputs } from "./taskCapabilities";
import { classifyEdge } from "./edgeClassify";

type CycleError = { ok: false; message: string; cycleNodes: string[]; };
type Ok = { ok: true; jcwf: JcwfFile; };

function buildAdjacency(graph: EditorGraph): Map<string, string[]>
{
  const adjacency = new Map<string, string[]>();
  for (const node of graph.nodes)
  {
    adjacency.set(node.id, []);
  }

  for (const edge of graph.edges)
  {
    // Ignore dataflow edges (slot wiring), filter fanout edges, and artifact-file edges (an external
    // file feeding a task — U3; a file node never participates in an orchestration cycle). Classified
    // via the shared classifier (U2) rather than re-reading the id prefix.
    const cls = classifyEdge(edge.sourceHandle, edge.targetHandle, edge.source, edge.target);
    if (cls === "dataflow" || cls === "fanout" || edge.source.startsWith("file:"))
    {
      continue;
    }

    // source -> target
    const list = adjacency.get(edge.source);
    if (list)
    {
      list.push(edge.target);
    }
  }

  return adjacency;
}

function findCycleNodes(graph: EditorGraph): string[]
{
  // Collect all nodes that participate in at least one cycle.
  // This is intentionally conservative (it may include multiple cycles).
  const adjacency = buildAdjacency(graph);

  // 0 = unvisited, 1 = visiting (in recursion stack), 2 = done
  const state = new Map<string, number>();
  const stack: string[] = [];
  const indexInStack = new Map<string, number>();
  const cycleNodes = new Set<string>();

  function markCycleFromStack(startNode: string): void
  {
    const startIndex = indexInStack.get(startNode);
    if (startIndex === undefined)
    {
      // Should not happen, but stay safe.
      cycleNodes.add(startNode);
      return;
    }

    for (let i = startIndex; i < stack.length; i += 1)
    {
      cycleNodes.add(stack[i]);
    }
  }

  function dfs(node: string): void
  {
    const currentState = state.get(node) ?? 0;
    if (currentState === 2)
    {
      return;
    }
    if (currentState === 1)
    {
      // Back-edge into current recursion stack.
      markCycleFromStack(node);
      return;
    }

    state.set(node, 1);
    indexInStack.set(node, stack.length);
    stack.push(node);

    const neighbors = adjacency.get(node) ?? [];
    for (const next of neighbors)
    {
      const nextState = state.get(next) ?? 0;
      if (nextState === 1)
      {
        // Back-edge: node -> next
        markCycleFromStack(next);
        continue;
      }
      dfs(next);
    }

    stack.pop();
    indexInStack.delete(node);
    state.set(node, 2);
  }

  for (const nodeId of adjacency.keys())
  {
    dfs(nodeId);
  }

  return Array.from(cycleNodes);
}

export function graphToJcwf(graph: EditorGraph, workflowId: string): Ok | CycleError
{
  const cycleNodes = findCycleNodes(graph);
  if (cycleNodes.length > 0)
  {
    return { ok: false, message: "Cycle detected. Export aborted.", cycleNodes };
  }

  // Separate task nodes, filter nodes, and control nodes
  const taskNodes = (graph.nodes as EditorNode[]).filter((n): n is EditorTaskNode => n.type === "task");
  const filterNodes = (graph.nodes as EditorNode[]).filter((n): n is EditorFilterNode => n.type === "filter");
  const controlNodes = (graph.nodes as EditorNode[]).filter((n): n is EditorControlNode => n.type === "branch");

  const tasks: Record<string, JcwfTask> = {};
  const sortedNodes = [...taskNodes].sort((a, b) => a.id.localeCompare(b.id));
  for (const node of sortedNodes)
  {
    const task = { ...(node.data.task as JcwfTask) };
    task.id = node.id;
    tasks[node.id] = task;
  }

  // Collect filters sorted by id
  const filters: JcwfFilter[] = filterNodes
    .map((n) => ({ ...n.data.filter }))
    .sort((a, b) => a.id.localeCompare(b.id));

  // Collect control_nodes sorted by id
  const control_nodes: JcwfControlNode[] = controlNodes
    .map((n) => ({ ...(n.data.controlNode as JcwfControlNode), id: n.id }))
    .sort((a, b) => a.id.localeCompare(b.id));

  // compute depends_on from edges
  for (const taskId of Object.keys(tasks))
  {
    delete tasks[taskId].depends_on;
  }

  const dataflow: JcwfDataflowEntry[] = [];
  const controlflow: JcwfControlflowEdge[] = [];

  // Collect dep entries with file_input handle indices for ordering.
  const depsByTask = new Map<string, Array<{ source: string; handleIdx: number }>>();

  for (const edge of graph.edges as EditorTaskEdge[])
  {
    // One classifier decides the edge's meaning from its handles + endpoints (U2 / S5); the id prefix
    // is just the persisted encoding of this.
    const cls = classifyEdge(edge.sourceHandle, edge.targetHandle, edge.source, edge.target);

    // Dataflow edge — named output/input slots.
    if (cls === "dataflow" && edge.sourceHandle && edge.targetHandle)
    {
      const fromOutput = edge.sourceHandle.startsWith("out:") ? edge.sourceHandle.slice(4) : edge.sourceHandle;
      const toInput = edge.targetHandle.startsWith("in:") ? edge.targetHandle.slice(3) : edge.targetHandle;
      dataflow.push({
        from_task: edge.source,
        from_output: fromOutput,
        to_task: edge.target,
        to_input: toInput,
      });
      continue;
    }

    // Skip fanout edges (auto-generated from filter → per_item task; binding lives in task.filter).
    if (cls === "fanout")
    {
      continue;
    }

    // Skip artifact-file edges (file node → task input). The file node is a pure editor overlay (no
    // JCWF task); the consumer's file_inputs path is the persisted artifact, not this edge (U3). This
    // is a fileflow edge distinguished from a task dependency by its file-node source.
    if (edge.source.startsWith("file:"))
    {
      continue;
    }

    // Controlflow edge (branching) — persisted separately.
    if (cls === "controlflow" && edge.sourceHandle && edge.targetHandle)
    {
      const kind: JcwfControlflowKind =
        edge.sourceHandle === "cf-out-error" ? "on_error"
          : edge.sourceHandle === "cf-out-normal" ? "normal"
            : edge.sourceHandle === "error-signal" ? "error_signal"
              : edge.targetHandle === "cf-in-error" ? "error_signal"
                : "normal";

      controlflow.push({
        from: edge.source,
        to: edge.target,
        kind,
        from_port: edge.sourceHandle,
        to_port: edge.targetHandle,
      });
      continue;
    }

    // Only fileflow (task→task dependency) edges participate in depends_on.
    if (cls !== "fileflow")
    {
      continue;
    }

    // depends_on entries must reference real TASKS on both ends. A dep edge dragged from a
    // filter node (id `filter:<id>`) or a branch node onto a task would otherwise serialise as
    // `depends_on: ['filter:<id>']` — a dependency on a non-task that never completes, which the
    // runtime reports as a deadlock/cycle. The filter→per_item binding lives in task.filter, not here.
    const targetTask = tasks[edge.target];
    const sourceTask = tasks[edge.source];
    if (targetTask && sourceTask)
    {
      let list = depsByTask.get(edge.target);
      if (!list)
      {
        list = [];
        depsByTask.set(edge.target, list);
      }

      // Extract dep handle index for ordering (dephandle-0, dephandle-1, …)
      const handleIdx = typeof edge.targetHandle === "string" && edge.targetHandle.startsWith("dephandle-")
        ? parseInt(edge.targetHandle.slice(10), 10)
        : Infinity; // generic deps sort after indexed deps

      if (!list.find((d) => d.source === edge.source))
      {
        list.push({ source: edge.source, handleIdx });
      }
    }
  }

  // Build depends_on arrays, sorted by file_input handle index, then alphabetically.
  for (const [taskId, deps] of depsByTask)
  {
    deps.sort((a, b) => {
      if (a.handleIdx !== b.handleIdx) return a.handleIdx - b.handleIdx;
      return a.source.localeCompare(b.source);
    });
    tasks[taskId].depends_on = deps.map((d) => d.source);
  }

  // U1 — derive file_inputs from incoming data edges at SAVE time. The edge is the authoritative
  // hand-off: a port fed by an edge (a `dep:` edge from an upstream task output, or a `file:` edge
  // from an artifact-file node) gets its path synthesized from the producer, replacing whatever was
  // stored. This kills the stored-path drift cluster — nothing to keep in sync because the path no
  // longer lives on the task between edits. Ports with NO incoming edge keep their stored entry (an
  // external input that isn't an artifact node yet — S2 transition rule). Only file_inputs-reading
  // types (shell/python/internal) participate; ai_call reads via queue cntx, so its stored file_inputs
  // are left untouched. The path synth mirrors the backend TaskPathResolver (incl. the base-leaf-strip
  // for `<wfId>/…` working dirs), verified zero-drift against all five shipped examples.
  for (const taskId of Object.keys(tasks))
  {
    const task = tasks[taskId];
    if (!readsFileInputs(task.type)) continue;

    const consumerWd = (task.working_directory ?? "") as string;
    const stored = Array.isArray(task.file_inputs) ? [...(task.file_inputs as string[])] : [];

    const derivedByPort = new Map<number, string>();
    for (const edge of graph.edges as EditorTaskEdge[])
    {
      if (edge.target !== taskId) continue;
      if (typeof edge.targetHandle !== "string" || !edge.targetHandle.startsWith("dephandle-")) continue;
      const portIdx = parseInt(edge.targetHandle.slice(10), 10);
      if (!(portIdx >= 0)) continue;

      let derived = "";
      if (edge.id.startsWith("file:"))
      {
        const relPath = edge.source.startsWith("file:") ? edge.source.slice("file:".length) : "";
        if (relPath) derived = fileNodeInputPath(relPath, consumerWd, workflowId);
      }
      else
      {
        const srcTask = tasks[edge.source];
        if (srcTask) derived = deriveUpstreamOutputPaths(srcTask, task, workflowId, edge.sourceHandle)[0] ?? "";
      }
      if (derived.length > 0) derivedByPort.set(portIdx, derived);
    }

    if (derivedByPort.size === 0) continue; // no wired ports — leave stored file_inputs as authored

    const maxLen = Math.max(stored.length, Math.max(...derivedByPort.keys()) + 1);
    const merged: string[] = [];
    for (let i = 0; i < maxLen; i += 1)
    {
      merged.push(derivedByPort.has(i) ? derivedByPort.get(i)! : (i < stored.length ? stored[i] : ""));
    }
    while (merged.length > 0 && merged[merged.length - 1].trim().length === 0) merged.pop();
    task.file_inputs = merged.length > 0 ? merged : undefined;
  }

  // Ensure every ai_call task has a complete environment (STNG, TASK, CNTX).
  // If any category is missing or empty, add a file with a single whitespace.
  const requiredCategories: Array<"stng_files" | "task_files" | "cntx_files"> = ["stng_files", "task_files", "cntx_files"];
  for (const taskId of Object.keys(tasks))
  {
    const task = tasks[taskId];
    if (task.type !== "ai_call") continue;
    const qb = (task.queue_binding ?? {}) as Record<string, unknown>;
    let changed = false;
    for (const cat of requiredCategories)
    {
      const entries = qb[cat] as unknown[] | undefined;
      if (!entries || entries.length === 0)
      {
        const prefix = cat.replace("_files", "").toUpperCase();
        qb[cat] = [{ path: `${prefix}_default.txt`, content: " " }];
        changed = true;
      }
    }
    if (changed)
    {
      task.queue_binding = qb as JcwfTask["queue_binding"];
    }
  }

  // Normalize queue-binding entries: an inline entry (object) whose `path` looks like a file
  // *reference* (contains "/" or "..") and carries no content is really a ref — an inline entry
  // writes its content to that path inside the queue folder, so a traversal path there makes the
  // runtime resolve the expected reply at the wrong place (the file-activity watchdog then expires).
  // Re-write it as a ref string so it reads the existing file instead (F-25).
  const allQueueCategories: Array<"stng_files" | "task_files" | "cntx_files" | "prob_files"> =
    ["stng_files", "task_files", "cntx_files", "prob_files"];
  for (const taskId of Object.keys(tasks))
  {
    const task = tasks[taskId];
    if (task.type !== "ai_call") continue;
    const qb = (task.queue_binding ?? {}) as Record<string, unknown>;
    let changed = false;
    for (const cat of allQueueCategories)
    {
      const entries = qb[cat] as Array<unknown> | undefined;
      if (!Array.isArray(entries)) continue;
      const normalized = entries.map((entry) => {
        if (entry && typeof entry === "object" && "path" in entry)
        {
          const obj = entry as { path?: unknown; content?: unknown };
          const path = typeof obj.path === "string" ? obj.path : "";
          const content = typeof obj.content === "string" ? obj.content : "";
          const looksLikeRef = path.includes("/") || path.includes("..");
          if (looksLikeRef && content.trim().length === 0)
          {
            changed = true;
            return path; // collapse inline-empty-with-ref-path → ref string
          }
        }
        return entry;
      });
      if (changed) qb[cat] = normalized;
    }
    if (changed)
    {
      task.queue_binding = qb as JcwfTask["queue_binding"];
    }
  }

  const orderedTasks: Record<string, JcwfTask> = {};
  for (const taskId of Object.keys(tasks).sort((a, b) => a.localeCompare(b)))
  {
    orderedTasks[taskId] = tasks[taskId];
  }

  // Persist node positions so the layout survives save/reload.
  const editor_layout: Record<string, { x: number; y: number }> = {};
  for (const node of graph.nodes as EditorNode[])
  {
    editor_layout[node.id] = { x: Math.round(node.position.x), y: Math.round(node.position.y) };
  }

  // Clean dataflow edges: dedupe identical entries, and drop orphans whose referenced output/input
  // slot no longer exists on the producer/consumer — e.g. after a slot is renamed, stale edges with
  // the old name linger, and repeated drags pile up duplicates (both hit during dogfooding).
  const seenDataflow = new Set<string>();
  const cleanedDataflow = dataflow.filter((df) => {
    const key = `${df.from_task}.${df.from_output}->${df.to_task}.${df.to_input}`;
    if (seenDataflow.has(key)) return false;
    seenDataflow.add(key);
    const fromOutputs = tasks[df.from_task]?.outputs as Record<string, unknown> | undefined;
    const toInputs = tasks[df.to_task]?.inputs as Record<string, unknown> | undefined;
    const fromOk = !!fromOutputs && Object.prototype.hasOwnProperty.call(fromOutputs, df.from_output);
    const toOk = !!toInputs && Object.prototype.hasOwnProperty.call(toInputs, df.to_input);
    return fromOk && toOk;
  });

  const needsV11 = filters.length > 0 || control_nodes.length > 0 || controlflow.length > 0;

  const jcwf: JcwfFile = {
    version: needsV11 ? "1.1" : "1.0",
    id: workflowId,
    tasks: orderedTasks,
    ...(filters.length > 0 ? { filters } : {}),
    ...(cleanedDataflow.length > 0 ? { dataflow: cleanedDataflow } : {}),
    ...(control_nodes.length > 0 ? { control_nodes } : {}),
    ...(controlflow.length > 0 ? { controlflow } : {}),
    editor_layout,
  };

  return { ok: true, jcwf };
}

export function detectGraphCycle(graph: EditorGraph): string[]
{
  return findCycleNodes(graph);
}
