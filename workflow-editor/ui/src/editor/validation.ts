import type { EditorGraph, ValidationResult } from "./types";
import { detectGraphCycle } from "./graphToJcwf";

export function validateGraph(graph: EditorGraph): ValidationResult
{
  const nodeErrorsById = new Map<string, string[]>();

  // id uniqueness (React Flow already needs unique ids, but we validate anyway)
  const ids = graph.nodes.map((n) => n.id);
  const idCounts = new Map<string, number>();
  for (const id of ids)
  {
    idCounts.set(id, (idCounts.get(id) ?? 0) + 1);
  }

  for (const node of graph.nodes)
  {
    const errors: string[] = [];

    if (!node.id || node.id.length === 0)
    {
      errors.push("Missing task id.");
    }
    if ((idCounts.get(node.id) ?? 0) > 1)
    {
      errors.push("Duplicate task id.");
    }

    const task = node.data.task;
    if (!task.type)
    {
      errors.push("Missing task.type.");
    }

    if (task.working_directory !== undefined && typeof task.working_directory !== "string")
    {
      errors.push("working_directory must be a string.");
    }

    if (errors.length > 0)
    {
      nodeErrorsById.set(node.id, errors);
    }
  }

  const cycleNodes = detectGraphCycle(graph);
  for (const nodeId of cycleNodes)
  {
    const current = nodeErrorsById.get(nodeId) ?? [];
    current.push("Cycle detected in depends_on graph.");
    nodeErrorsById.set(nodeId, current);
  }

  return { nodeErrorsById, cycleNodes };
}
