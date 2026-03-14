import type { EditorGraph, ValidationResult } from "./types";
import { detectGraphCycle } from "./graphToJcwf";

export function validateGraph(graph: EditorGraph): ValidationResult
{
  const nodeErrorsById = new Map<string, string[]>();
  const nodeWarningsById = new Map<string, string[]>();
  const nodeInfosById = new Map<string, string[]>();

  // id uniqueness (React Flow already needs unique ids, but we validate anyway)
  const ids = graph.nodes.map((n) => n.id);
  const idCounts = new Map<string, number>();
  for (const id of ids)
  {
    idCounts.set(id, (idCounts.get(id) ?? 0) + 1);
  }

  for (const node of graph.nodes)
  {
    // Filter nodes are not validated here
    if (node.type === "filter" || node.type === "branch")
    {
      continue;
    }

    const errors: string[] = [];
    const warnings: string[] = [];
    const infos: string[] = [];

    if (!node.id || node.id.length === 0)
    {
      errors.push("Missing task id.");
    }
    if ((idCounts.get(node.id) ?? 0) > 1)
    {
      errors.push("Duplicate task id.");
    }

    const task = (node.data as { task: { type?: string; working_directory?: string } }).task;
    if (!task.type)
    {
      errors.push("Missing task.type.");
    }

    // working_directory policy (keep this slightly permissive on the client; backend is authoritative)
    if (task.working_directory === "")
    {
      errors.push("working_directory must not be empty.");
    }
    else if (task.working_directory === undefined)
    {
      infos.push("working_directory is not set.");
    }

    if (task.working_directory !== undefined && typeof task.working_directory !== "string")
    {
      errors.push("working_directory must be a string.");
    }

    // Shell task: command must start with "scripts/" and the resolved path
    // must remain inside scripts/ after normalizing ".." (JCWF spec §3.1.2).
    if (task.type === "shell")
    {
      const params = (task as { params?: Record<string, unknown> }).params;
      const command = typeof params?.command === "string" ? params.command : "";
      if (command.length > 0)
      {
        if (!command.startsWith("scripts/"))
        {
          errors.push("Shell command must be inside the 'scripts/' directory (JCWF spec §3.1.2).");
        }
        else
        {
          // Lexically normalize: resolve ".." segments and check the result
          const parts = command.split("/");
          const resolved: string[] = [];
          for (const p of parts)
          {
            if (p === ".." && resolved.length > 0 && resolved[resolved.length - 1] !== "..")
            {
              resolved.pop();
            }
            else if (p !== ".")
            {
              resolved.push(p);
            }
          }
          const normalized = resolved.join("/");
          if (!normalized.startsWith("scripts/"))
          {
            errors.push("Resolved script path escapes the 'scripts/' directory (JCWF spec §3.1.2).");
          }
        }
      }
    }

    // timeout_ms validation
    const taskTimeout = (task as { timeout_ms?: number }).timeout_ms;
    if (taskTimeout !== undefined && typeof taskTimeout === "number" && taskTimeout < 0)
    {
      errors.push("timeout_ms must not be negative.");
    }

    // Materialize validation for shell/python tasks
    const materializeMap = (task as { materialize?: Record<string, string> }).materialize;
    if (materializeMap && typeof materializeMap === "object")
    {
      const entries = Object.entries(materializeMap);
      for (let i = 0; i < entries.length; i++)
      {
        const [src, tgt] = entries[i];
        if (!src || src.trim().length === 0)
        {
          warnings.push(`materialize row ${i + 1}: source is empty.`);
        }
        if (!tgt || tgt.trim().length === 0)
        {
          warnings.push(`materialize row ${i + 1}: target filename is empty.`);
        }
      }
    }

    if (errors.length > 0)
    {
      nodeErrorsById.set(node.id, errors);
    }

    if (warnings.length > 0)
    {
      nodeWarningsById.set(node.id, warnings);
    }

    if (infos.length > 0)
    {
      nodeInfosById.set(node.id, infos);
    }
  }

  // Edge sanity checks
  const nodeIdSet = new Set<string>(graph.nodes.map((n) => n.id));
  for (const edge of graph.edges)
  {
    if (edge.source === edge.target)
    {
      const current = nodeErrorsById.get(edge.target) ?? [];
      current.push("Self-dependency is not allowed (task depends_on itself).");
      nodeErrorsById.set(edge.target, current);
      continue;
    }

    if (!nodeIdSet.has(edge.source) || !nodeIdSet.has(edge.target))
    {
      // This shouldn't normally happen, but can occur briefly during edits.
      const targetId = nodeIdSet.has(edge.target) ? edge.target : edge.source;
      const current = nodeErrorsById.get(targetId) ?? [];
      current.push("Edge references a missing node.");
      nodeErrorsById.set(targetId, current);
    }
  }

  const cycleNodes = detectGraphCycle(graph);
  for (const nodeId of cycleNodes)
  {
    const current = nodeErrorsById.get(nodeId) ?? [];
    current.push("Cycle detected in depends_on graph.");
    nodeErrorsById.set(nodeId, current);
  }

  return { nodeErrorsById, nodeWarningsById, nodeInfosById, cycleNodes };
}
