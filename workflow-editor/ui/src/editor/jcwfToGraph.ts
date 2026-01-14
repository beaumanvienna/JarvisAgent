import { Position } from "reactflow";
import type { EditorGraph, EditorTaskNode, EditorTaskEdge } from "./types";
import type { JcwfWorkflow } from "../jcwf/types";

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

export function jcwfToGraph(workflow: JcwfWorkflow): EditorGraph
{
  const taskIds = Object.keys(workflow.tasks);

  const edgesPairs: Array<[string, string]> = [];
  for (const [taskKey, task] of Object.entries(workflow.tasks))
  {
    const dependsOn = task.depends_on ?? [];
    for (const dep of dependsOn)
    {
      // dep -> taskKey
      edgesPairs.push([dep, taskKey]);
    }
  }

  const cycleResult = detectCycle(taskIds, edgesPairs);
  if (!cycleResult.ok)
  {
    throw new Error(`JCWF graph is not a DAG. Cycle detected: ${cycleResult.cyclePath.join(" -> ")}`);
  }

  const nodes: EditorTaskNode[] = taskIds.map((taskId, index) =>
  {
    const task = workflow.tasks[taskId];
    const title = task.label && task.label.length > 0 ? task.label : task.id;
    const subtitle = task.type;

    return {
      id: taskId,
      type: "task",
      position: { x: 200 * index, y: 80 },
      data: { title, subtitle },
      sourcePosition: Position.Right,
      targetPosition: Position.Left
    };
  });

  const edges: EditorTaskEdge[] = edgesPairs.map(([from, to], index) =>
  {
    return {
      id: `e-${from}-${to}-${index}`,
      source: from,
      target: to,
      type: "smoothstep"
    };
  });

  return { nodes, edges };
}
