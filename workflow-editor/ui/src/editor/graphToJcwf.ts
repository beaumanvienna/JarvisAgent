import type { EditorGraph, EditorFilterNode, EditorTaskEdge, EditorTaskNode, EditorNode } from "./types";
import type { JcwfDataflowEntry, JcwfFile, JcwfFilter, JcwfTask } from "../jcwf/types";

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

  // Separate task nodes and filter nodes
  const taskNodes = (graph.nodes as EditorNode[]).filter((n): n is EditorTaskNode => n.type === "task");
  const filterNodes = (graph.nodes as EditorNode[]).filter((n): n is EditorFilterNode => n.type === "filter");

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

  // compute depends_on from edges
  for (const taskId of Object.keys(tasks))
  {
    delete tasks[taskId].depends_on;
  }

  const dataflow: JcwfDataflowEntry[] = [];

  // Collect dep entries with file_input handle indices for ordering.
  const depsByTask = new Map<string, Array<{ source: string; handleIdx: number }>>();

  for (const edge of graph.edges as EditorTaskEdge[])
  {
    // Dataflow edges have df: prefix and encode output/input in sourceHandle/targetHandle
    if (edge.id.startsWith("df:") && edge.sourceHandle && edge.targetHandle)
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

    // Skip fanout edges (auto-generated from filter → per_item task)
    if (edge.id.startsWith("fanout:"))
    {
      continue;
    }

    const targetTask = tasks[edge.target];
    if (targetTask)
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

  const orderedTasks: Record<string, JcwfTask> = {};
  for (const taskId of Object.keys(tasks).sort((a, b) => a.localeCompare(b)))
  {
    orderedTasks[taskId] = tasks[taskId];
  }

  const hasFilters = filters.length > 0;

  const jcwf: JcwfFile = {
    version: hasFilters ? "1.1" : "1.0",
    id: workflowId,
    tasks: orderedTasks,
    ...(hasFilters ? { filters } : {}),
    ...(dataflow.length > 0 ? { dataflow } : {}),
  };

  return { ok: true, jcwf };
}

export function detectGraphCycle(graph: EditorGraph): string[]
{
  return findCycleNodes(graph);
}
