import type { EditorGraph } from "./types";
import type { JcwfWorkflow, JcwfTaskDef } from "../jcwf/types";

type CycleCheckResult = { ok: true } | { ok: false; cyclePath: string[] };

function detectCycle(taskIds: string[], edges: Array<[string, string]>): CycleCheckResult
{
  const adjacency = new Map<string, string[]>();
  for (const id of taskIds)
  {
    adjacency.set(id, []);
  }
  for (const [from, to] of edges)
  {
    if (!adjacency.has(from))
    {
      adjacency.set(from, []);
    }
    adjacency.get(from)!.push(to);
  }

  const visiting = new Set<string>();
  const visited = new Set<string>();
  const stack: string[] = [];

  const dfs = (node: string): CycleCheckResult =>
  {
    if (visiting.has(node))
    {
      const cycleStartIndex = stack.indexOf(node);
      const cyclePath = cycleStartIndex >= 0 ? stack.slice(cycleStartIndex).concat([node]) : [node, node];
      return { ok: false, cyclePath };
    }
    if (visited.has(node))
    {
      return { ok: true };
    }

    visiting.add(node);
    stack.push(node);

    const neighbors = adjacency.get(node) ?? [];
    for (const next of neighbors)
    {
      const result = dfs(next);
      if (!result.ok)
      {
        return result;
      }
    }

    stack.pop();
    visiting.delete(node);
    visited.add(node);
    return { ok: true };
  };

  for (const id of taskIds)
  {
    const result = dfs(id);
    if (!result.ok)
    {
      return result;
    }
  }

  return { ok: true };
}

export function graphToJcwf(graph: EditorGraph, workflowId: string): JcwfWorkflow
{
  const tasks: Record<string, JcwfTaskDef> = {};

  const taskIds = graph.nodes.map((n) => n.id);
  for (const node of graph.nodes)
  {
    const taskType = node.data?.subtitle ?? "internal";
    const label = node.data?.title ?? node.id;

    tasks[node.id] = {
      id: node.id,
      type: taskType,
      label,
      depends_on: []
    };
  }

  // edges represent depends_on: source -> target
  for (const edge of graph.edges)
  {
    if (tasks[edge.target] && tasks[edge.source])
    {
      tasks[edge.target].depends_on = tasks[edge.target].depends_on ?? [];
      tasks[edge.target].depends_on!.push(edge.source);
    }
  }

  const edgesPairs: Array<[string, string]> = graph.edges.map((e) => [e.source, e.target]);
  const cycleResult = detectCycle(taskIds, edgesPairs);
  if (!cycleResult.ok)
  {
    throw new Error(`Editor graph is not a DAG. Cycle detected: ${cycleResult.cyclePath.join(" -> ")}`);
  }

  return {
    version: "1.0",
    id: workflowId,
    tasks
  };
}
