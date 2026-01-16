import type { EditorGraph, EditorTaskEdge, EditorTaskNode } from "./types";
import type { JcwfFile, JcwfTask } from "../jcwf/types";

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

  const tasks: Record<string, JcwfTask> = {};
  const sortedNodes = [...(graph.nodes as EditorTaskNode[])].sort((a, b) => a.id.localeCompare(b.id));
  for (const node of sortedNodes)
  {
    const task = { ...(node.data.task as JcwfTask) };
    task.id = node.id;
    tasks[node.id] = task;
  }

  // compute depends_on from edges
  for (const taskId of Object.keys(tasks))
  {
    delete tasks[taskId].depends_on;
  }

  for (const edge of graph.edges as EditorTaskEdge[])
  {
    const targetTask = tasks[edge.target];
    if (targetTask)
    {
      if (!Array.isArray(targetTask.depends_on))
      {
        targetTask.depends_on = [];
      }
      // Keep depends_on unique to avoid duplicate edges producing duplicate dependencies.
      if (!targetTask.depends_on.includes(edge.source))
      {
        targetTask.depends_on.push(edge.source);
      }
    }
  }

  // Deterministic depends_on ordering.
  for (const taskId of Object.keys(tasks))
  {
    const deps = tasks[taskId].depends_on;
    if (Array.isArray(deps))
    {
      deps.sort((a, b) => a.localeCompare(b));
    }
  }

  const orderedTasks: Record<string, JcwfTask> = {};
  for (const taskId of Object.keys(tasks).sort((a, b) => a.localeCompare(b)))
  {
    orderedTasks[taskId] = tasks[taskId];
  }

  const jcwf: JcwfFile = {
    version: "1.0",
    id: workflowId,
    tasks: orderedTasks,
  };

  return { ok: true, jcwf };
}

export function detectGraphCycle(graph: EditorGraph): string[]
{
  return findCycleNodes(graph);
}
