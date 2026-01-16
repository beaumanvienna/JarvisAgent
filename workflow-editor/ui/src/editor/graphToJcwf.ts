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
  const adjacency = buildAdjacency(graph);
  const visiting = new Set<string>();
  const visited = new Set<string>();
  const cycleNodes = new Set<string>();

  function dfs(node: string): void
  {
    if (visiting.has(node))
    {
      cycleNodes.add(node);
      return;
    }
    if (visited.has(node))
    {
      return;
    }

    visiting.add(node);
    const neighbors = adjacency.get(node) ?? [];
    for (const next of neighbors)
    {
      dfs(next);
    }
    visiting.delete(node);
    visited.add(node);
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
  for (const node of graph.nodes as EditorTaskNode[])
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
      targetTask.depends_on.push(edge.source);
    }
  }

  const jcwf: JcwfFile = {
    version: "1.0",
    id: workflowId,
    tasks,
  };

  return { ok: true, jcwf };
}

export function detectGraphCycle(graph: EditorGraph): string[]
{
  return findCycleNodes(graph);
}
