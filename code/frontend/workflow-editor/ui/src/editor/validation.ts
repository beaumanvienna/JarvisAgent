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

    // working_directory policy (keep this slightly permissive on the client; backend is authoritative).
    // An empty string behaves identically to "unset" at runtime (defaults to the workflow folder),
    // so it's a Tier-D info, not a blocking error — the field can be cleared without a red error.
    if (task.working_directory === "" || task.working_directory === undefined)
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

    // Internal task: requires params.action naming a registered C++ factory.
    if (task.type === "internal")
    {
      const params = (task as { params?: Record<string, unknown> }).params;
      const action = typeof params?.action === "string" ? params.action.trim() : "";
      if (action.length === 0)
      {
        errors.push("Internal task requires params.action (e.g. carMaintenance).");
      }
    }

    // Structured output (ai_call): output_schema must be a JSON object; output_retries a
    // non-negative integer. The inspector blocks committing unparseable JSON, but a loaded
    // workflow could still carry a malformed value.
    const outputSchema = (task as { output_schema?: unknown }).output_schema;
    if (outputSchema !== undefined && outputSchema !== null
      && (typeof outputSchema !== "object" || Array.isArray(outputSchema)))
    {
      warnings.push("output_schema should be a JSON object (a Draft 2020-12 schema).");
    }
    const outputRetries = (task as { output_retries?: unknown }).output_retries;
    if (outputRetries !== undefined
      && (typeof outputRetries !== "number" || outputRetries < 0 || !Number.isInteger(outputRetries)))
    {
      warnings.push("output_retries must be a non-negative integer.");
    }

    // F-14: a shell/python task with file I/O but no explicit args gets the backend's auto-injected
    // "inputs-first, outputs-last" args ({{input[0]}} … {{output[0]}}, EnsureDefaultInputOutputArgs).
    // Archive-first tools (zip/tar take the archive as $1) need the opposite order and otherwise fail
    // at runtime with no editor-time signal. Inform so the user sets explicit args when order matters.
    if (task.type === "shell" || task.type === "python")
    {
      const fileInputs = (task as { file_inputs?: unknown }).file_inputs;
      const fileOutputs = (task as { file_outputs?: unknown }).file_outputs;
      const hasFileIo = (Array.isArray(fileInputs) && fileInputs.length > 0)
        || (Array.isArray(fileOutputs) && fileOutputs.length > 0);
      const args = (task as { params?: { args?: unknown } }).params?.args;
      const hasArgs = Array.isArray(args) && args.length > 0;
      if (hasFileIo && !hasArgs)
      {
        infos.push("args is empty → auto-injected as inputs-first, outputs-last ({{input[0]}} … {{output[0]}}). Set explicit args if your tool needs another order (e.g. archive-first zip/tar).");
      }
    }

    // F-40: a per_item ai_call whose PROB *path* has no per-row template makes every fan-out row
    // materialise to the same file (PROB_new_1.{txt,output.txt}) — they race, the file-activity
    // watchdog expires, and the run fails with no editor-time signal. A unique path per row requires
    // a template token ({{<binding>.*}}, {{row_number}}, {{row_number_padded}}) in the path.
    if (task.type === "ai_call" && (task as { mode?: string }).mode === "per_item")
    {
      const qb = (task as { queue_binding?: { prob_files?: Array<string | { path?: string }> } }).queue_binding;
      const probFiles = Array.isArray(qb?.prob_files) ? qb!.prob_files! : [];
      for (const entry of probFiles)
      {
        const p = typeof entry === "string" ? entry : (entry?.path ?? "");
        if (p.length > 0 && !p.includes("{{"))
        {
          warnings.push(`per_item PROB path '${p}' has no per-row template — every fan-out row writes to the same file and the run will collide/fail. Add a token like {{row_number_padded}} or {{<binding>.<field>}} to the filename.`);
        }
      }
    }

    // Dataflow slots: declared input/output slot names must be non-empty (an empty key can't
    // be wired and breaks the dataflow reference).
    for (const slotKey of ["inputs", "outputs"] as const)
    {
      const slots = (task as Record<string, unknown>)[slotKey];
      if (slots && typeof slots === "object" && !Array.isArray(slots))
      {
        for (const name of Object.keys(slots as Record<string, unknown>))
        {
          if (name.trim().length === 0)
          {
            warnings.push(`Dataflow ${slotKey} slot has an empty name.`);
          }
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

  // Colliding AI output paths: two ai_call tasks sharing a working_directory AND a PROB
  // filename register the same expected-output path (PROB_*.output.txt). Only one resolves;
  // the others hang in waiting_external or fail. Warn so it's caught before a run.
  const aiOutputKeys = new Map<string, string[]>();
  for (const node of graph.nodes)
  {
    if (node.type !== "task")
    {
      continue;
    }
    const t = (node.data as { task: { type?: string; working_directory?: string; queue_binding?: { prob_files?: Array<string | { path?: string }> } } }).task;
    if (t?.type !== "ai_call")
    {
      continue;
    }
    const wd = (t.working_directory ?? "").replace(/\/+$/, "");
    const probFiles = Array.isArray(t.queue_binding?.prob_files) ? t.queue_binding!.prob_files! : [];
    const firstProb = probFiles.length > 0
      ? (typeof probFiles[0] === "string" ? probFiles[0] : (probFiles[0]?.path ?? ""))
      : "";
    const key = `${wd}::${firstProb}`;
    const list = aiOutputKeys.get(key) ?? [];
    list.push(node.id);
    aiOutputKeys.set(key, list);
  }
  for (const nodeIds of aiOutputKeys.values())
  {
    if (nodeIds.length < 2)
    {
      continue;
    }
    for (const id of nodeIds)
    {
      const others = nodeIds.filter((x) => x !== id);
      const current = nodeWarningsById.get(id) ?? [];
      current.push(`Colliding AI output path with ${others.join(", ")} (same working_directory + PROB filename). Give each AI task a distinct working_directory or PROB filename, or runs will hang/fail.`);
      nodeWarningsById.set(id, current);
    }
  }

  // F-33: two ai_call tasks with identical PROB content are almost always a copy-paste mistake
  // (the wrong PROB pasted into a duplicated node) — it surfaces only at run as a duplicated/missing
  // result. Info so it's caught at edit time. Keyed on the concatenated inline PROB content; ref-only
  // entries (no inline content) are skipped since they don't carry comparable text here.
  const aiProbContentKeys = new Map<string, string[]>();
  for (const node of graph.nodes)
  {
    if (node.type !== "task")
    {
      continue;
    }
    const t = (node.data as { task: { type?: string; queue_binding?: { prob_files?: Array<string | { content?: string }> } } }).task;
    if (t?.type !== "ai_call")
    {
      continue;
    }
    const probFiles = Array.isArray(t.queue_binding?.prob_files) ? t.queue_binding!.prob_files! : [];
    const content = probFiles
      .map((e) => (typeof e === "string" ? "" : (e?.content ?? "")))
      .join(" ")
      .trim();
    if (content.length === 0)
    {
      continue;
    }
    const list = aiProbContentKeys.get(content) ?? [];
    list.push(node.id);
    aiProbContentKeys.set(content, list);
  }
  for (const nodeIds of aiProbContentKeys.values())
  {
    if (nodeIds.length < 2)
    {
      continue;
    }
    for (const id of nodeIds)
    {
      const others = nodeIds.filter((x) => x !== id);
      const current = nodeInfosById.get(id) ?? [];
      current.push(`Identical PROB content as ${others.join(", ")} — if these AI tasks should differ (e.g. distinct codes/items), one was likely pasted without updating its PROB.`);
      nodeInfosById.set(id, current);
    }
  }

  return { nodeErrorsById, nodeWarningsById, nodeInfosById, cycleNodes };
}
