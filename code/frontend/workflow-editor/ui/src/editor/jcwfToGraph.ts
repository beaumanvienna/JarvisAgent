import type { CSSProperties } from "react";
import type { EditorControlNode, EditorGraph, EditorFileNode, EditorFilterNode, EditorTaskEdge, EditorTaskNode, EditorNode } from "./types";
import type { JcwfControlNode, JcwfControlflowEdge, JcwfDataflowEntry, JcwfFile, JcwfFilter, JcwfTask } from "../jcwf/types";
import { FILE_INPUT_COLORS } from "./constants";
import { normalizePathSegments, resolveTaskDirSegments, deriveUpstreamOutputPaths } from "./pathSynthesis";
import { readsFileInputs } from "./taskCapabilities";

function displayTitle(task: JcwfTask): { title: string; subtitle?: string }
{
  return {
    title: task.label && task.label.length > 0 ? task.label : task.id,
    subtitle: task.mode === "per_item" ? `${task.type} (per_item)` : task.type,
  };
}

function filterDisplayTitle(filter: JcwfFilter): { title: string; subtitle?: string }
{
  return {
    title: filter.id,
    subtitle: filter.source.kind,
  };
}

// `workflowId` is passed explicitly (the editor always knows it from the URL) rather than trusting
// `jcwf.id` — some extracted canvases omit the id field, and the file-node inference (S3) needs the
// real workflow id to resolve workflows/<id>/ paths. Falls back to jcwf.id for callers that don't pass it.
export function jcwfToGraph(jcwf: JcwfFile, workflowId?: string): EditorGraph
{
  const taskEntries = Object.entries(jcwf.tasks ?? {});
  const taskIdSet = new Set<string>(taskEntries.map(([taskId]) => taskId));
  const cellW = 320;
  const VERTICAL_GAP = 30;
  const BASE_NODE_HEIGHT = 60;
  const DEP_ROW_HEIGHT = 15;

  // Persisted editor layout from the JCWF (if present).
  const savedLayout = (jcwf as Record<string, unknown>).editor_layout as
    Record<string, { x: number; y: number }> | undefined;

  // Simple layout: assign a "level" per task based on depends_on depth (DAG). If cycles exist,
  // we fall back to a stable insertion order layout.
  const depsByTaskId = new Map<string, string[]>();
  for (const [taskId, taskValue] of taskEntries)
  {
    const task = taskValue as JcwfTask;
    const deps = Array.isArray(task.depends_on) ? task.depends_on.filter((d) => taskIdSet.has(d)) : [];
    depsByTaskId.set(taskId, deps);
  }

  function computeLevels(): Map<string, number>
  {
    const levels = new Map<string, number>();
    const visiting = new Set<string>();
    const visited = new Set<string>();

    function dfs(taskId: string): number
    {
      if (visited.has(taskId))
      {
        return levels.get(taskId) ?? 0;
      }
      if (visiting.has(taskId))
      {
        // Cycle; treat as 0 so layout stays bounded.
        return 0;
      }

      visiting.add(taskId);
      const deps = depsByTaskId.get(taskId) ?? [];
      let level = 0;
      for (const dep of deps)
      {
        level = Math.max(level, dfs(dep) + 1);
      }
      visiting.delete(taskId);

      visited.add(taskId);
      levels.set(taskId, level);
      return level;
    }

    for (const [taskId] of taskEntries)
    {
      dfs(taskId);
    }

    return levels;
  }

  const levels = computeLevels();

  const taskIdsByLevel = new Map<number, string[]>();
  for (const [taskId] of taskEntries)
  {
    const level = levels.get(taskId) ?? 0;
    const list = taskIdsByLevel.get(level) ?? [];
    list.push(taskId);
    taskIdsByLevel.set(level, list);
  }

  // Deterministic ordering within each level.
  for (const [level, list] of taskIdsByLevel.entries())
  {
    const sorted = [...list].sort((a, b) => a.localeCompare(b));
    taskIdsByLevel.set(level, sorted);
  }

  // Estimate node height based on dep handle count
  function estimateNodeHeight(taskId: string): number
  {
    const task = Object.fromEntries(taskEntries)[taskId] as JcwfTask | undefined;
    if (!task) return BASE_NODE_HEIGHT;
    const fileInputs = Array.isArray(task.file_inputs) ? task.file_inputs.length : 0;
    const deps = (depsByTaskId.get(taskId) ?? []).length;
    const inputHandleCount = Math.max(fileInputs, deps);
    const fileOutputs = Array.isArray(task.file_outputs) ? task.file_outputs.length : 0;
    return BASE_NODE_HEIGHT + Math.max(inputHandleCount, fileOutputs) * DEP_ROW_HEIGHT;
  }

  const positionByTaskId = new Map<string, { x: number; y: number }>();
  const sortedLevels = Array.from(taskIdsByLevel.keys()).sort((a, b) => a - b);
  for (const level of sortedLevels)
  {
    const list = taskIdsByLevel.get(level) ?? [];
    let cumulativeY = 0;
    for (let row = 0; row < list.length; row += 1)
    {
      positionByTaskId.set(list[row], { x: level * cellW, y: cumulativeY });
      cumulativeY += estimateNodeHeight(list[row]) + VERTICAL_GAP;
    }
  }

  const taskNodes: EditorTaskNode[] = taskEntries.map(([taskId, taskValue]) => {
    const task = taskValue as JcwfTask;
    const { title, subtitle } = displayTitle(task);
    const saved = savedLayout?.[taskId];
    const position = saved ? { x: saved.x, y: saved.y } : (positionByTaskId.get(taskId) ?? { x: 0, y: 0 });
    return {
      id: taskId,
      type: "task" as const,
      position,
      data: { task, title, subtitle },
    };
  });

  // Control nodes (branch) — compute level from controlflow predecessors.
  const controlNodesRaw = Array.isArray((jcwf as Record<string, unknown>).control_nodes)
    ? ((jcwf as Record<string, unknown>).control_nodes as JcwfControlNode[])
    : [];

  const controlflowForLayout = Array.isArray((jcwf as Record<string, unknown>).controlflow)
    ? ((jcwf as Record<string, unknown>).controlflow as JcwfControlflowEdge[])
    : [];

  // For each control node, find the max level of its predecessor tasks (from controlflow edges)
  // and place the control node one level after that.  For output targets of the branch,
  // collect the max y of predecessor rows at the computed level for vertical stacking.
  const controlNodeLevels = new Map<string, number>();
  for (const cn of controlNodesRaw)
  {
    let maxPredLevel = 0;
    for (const cf of controlflowForLayout)
    {
      if (cf.to === cn.id && levels.has(cf.from))
      {
        maxPredLevel = Math.max(maxPredLevel, (levels.get(cf.from) ?? 0) + 1);
      }
    }
    controlNodeLevels.set(cn.id, maxPredLevel);
  }

  // Track how many items are already in each level column for vertical stacking
  const levelRowCount = new Map<number, number>();
  for (const [, list] of taskIdsByLevel)
  {
    for (const tid of list)
    {
      const lv = levels.get(tid) ?? 0;
      levelRowCount.set(lv, (levelRowCount.get(lv) ?? 0) + 1);
    }
  }

  const controlNodes: EditorControlNode[] = controlNodesRaw.map((cn) => {
    const cnLevel = controlNodeLevels.get(cn.id) ?? 0;
    const row = levelRowCount.get(cnLevel) ?? 0;
    levelRowCount.set(cnLevel, row + 1);
    const saved = savedLayout?.[cn.id];
    return {
      id: cn.id,
      type: "branch" as const,
      position: saved ? { x: saved.x, y: saved.y } : { x: cnLevel * cellW, y: row * (BASE_NODE_HEIGHT + VERTICAL_GAP) },
      data: {
        controlNode: cn,
        title: cn.label && cn.label.length > 0 ? cn.label : cn.id,
        subtitle: cn.type,
      },
    };
  });

  // Filter nodes — placed to the left of the leftmost task column
  const filters = Array.isArray(jcwf.filters) ? jcwf.filters : [];
  const filterNodes: EditorFilterNode[] = filters.map((filter, index) => {
    const { title, subtitle } = filterDisplayTitle(filter);
    const minX = sortedLevels.length > 0 ? (sortedLevels[0] * cellW) : 0;
    const filterId = `filter:${filter.id}`;
    const saved = savedLayout?.[filterId];
    return {
      id: filterId,
      type: "filter" as const,
      position: saved ? { x: saved.x, y: saved.y } : { x: minX - cellW, y: index * (BASE_NODE_HEIGHT + VERTICAL_GAP) },
      data: { filter, title, subtitle },
    };
  });

  const nodes: EditorNode[] = [...filterNodes, ...taskNodes, ...controlNodes];

  const edges: EditorTaskEdge[] = [];

  // Controlflow edges (branching)
  const nodeIdSet = new Set<string>(nodes.map((n) => n.id));
  const controlflowEntries = Array.isArray((jcwf as Record<string, unknown>).controlflow)
    ? ((jcwf as Record<string, unknown>).controlflow as JcwfControlflowEdge[])
    : [];
  for (const cf of controlflowEntries)
  {
    if (!nodeIdSet.has(cf.from) || !nodeIdSet.has(cf.to))
    {
      continue;
    }

    const sourceHandle = cf.from_port;
    const targetHandle = cf.to_port;

    const style: CSSProperties = cf.kind === "error_signal"
      ? { strokeDasharray: "2 3", stroke: "rgba(255,120,120,0.85)", strokeWidth: 2 }
      : cf.kind === "on_error"
        ? { strokeDasharray: "6 4", stroke: "rgba(255,120,120,0.85)", strokeWidth: 2 }
        : { strokeDasharray: "6 4", stroke: "rgba(255,200,140,0.9)", strokeWidth: 2 };

    const label = cf.kind === "error_signal" ? "error" : (cf.kind === "on_error" ? "on_error" : "normal");

    edges.push({
      id: `cf:${cf.from}->${cf.to}:${cf.kind}`,
      source: cf.from,
      target: cf.to,
      sourceHandle,
      targetHandle,
      style,
      label,
      labelStyle: { fill: "rgba(255,220,180,0.95)", fontSize: 10, fontWeight: 600 },
      labelBgStyle: { fill: "rgba(30,35,45,0.92)", stroke: "rgba(255,180,80,0.3)", strokeWidth: 1 },
      labelBgPadding: [4, 6] as [number, number],
      labelBgBorderRadius: 4,
    });
  }

  for (const [taskId, taskValue] of taskEntries)
  {
    const task = taskValue as JcwfTask;
    const deps = Array.isArray(task.depends_on) ? task.depends_on : [];

    for (let depIdx = 0; depIdx < deps.length; depIdx++)
    {
      const depId = deps[depIdx];
      if (!taskIdSet.has(depId))
      {
        // Ignore invalid dependencies. Backend validation is authoritative.
        continue;
      }

      const color = FILE_INPUT_COLORS[depIdx % FILE_INPUT_COLORS.length];

      // Determine which output handle the edge should leave from on the source task.
      const sourceTask = Object.fromEntries(taskEntries)[depId] as JcwfTask | undefined;
      const srcOutputs: string[] = sourceTask && Array.isArray(sourceTask.file_outputs) ? sourceTask.file_outputs as string[] : [];
      let sourceHandle = "dep-source";
      if (srcOutputs.length === 1)
      {
        sourceHandle = "fileoutput-0";
      }
      else if (srcOutputs.length > 1)
      {
        // Try to match by filename: target's file_input[depIdx] vs source's file_outputs
        const targetFileInputs: string[] = Array.isArray(task.file_inputs) ? task.file_inputs as string[] : [];
        if (depIdx < targetFileInputs.length)
        {
          const targetFile = targetFileInputs[depIdx].split("/").pop() ?? "";
          const matchIdx = srcOutputs.findIndex((o) => (o.split("/").pop() ?? "") === targetFile);
          sourceHandle = matchIdx >= 0 ? `fileoutput-${matchIdx}` : "fileoutput-0";
        }
        else
        {
          sourceHandle = "fileoutput-0";
        }
      }

      edges.push({
        id: `dep:${depId}->${taskId}:${depIdx}`,
        source: depId,
        target: taskId,
        sourceHandle,
        targetHandle: `dephandle-${depIdx}`,
        style: { stroke: color, strokeWidth: 2 },
      });
    }

    // Auto fan-out edge: filter → per_item task
    if (task.mode === "per_item" && task.filter)
    {
      const filterNodeId = `filter:${task.filter}`;
      edges.push({
        id: `fanout:${filterNodeId}->${taskId}`,
        source: filterNodeId,
        target: taskId,
        style: { strokeDasharray: "6 3", stroke: "rgba(180, 140, 255, 0.7)" },
        label: "per_item",
        labelStyle: { fill: "rgba(180, 140, 255, 0.9)", fontSize: 10 },
      });
    }
  }

  // Dataflow edges (output → input wiring between tasks)
  const dataflowEntries = Array.isArray((jcwf as Record<string, unknown>).dataflow)
    ? ((jcwf as Record<string, unknown>).dataflow as JcwfDataflowEntry[])
    : [];
  for (const df of dataflowEntries)
  {
    if (!taskIdSet.has(df.from_task) || !taskIdSet.has(df.to_task))
    {
      continue;
    }
    edges.push({
      id: `df:${df.from_task}.${df.from_output}->${df.to_task}.${df.to_input}`,
      source: df.from_task,
      target: df.to_task,
      sourceHandle: `out:${df.from_output}`,
      targetHandle: `in:${df.to_input}`,
      style: { strokeDasharray: "5 4", stroke: "rgba(100, 210, 180, 0.7)" },
      label: `${df.from_output} → ${df.to_input}`,
      labelStyle: { fill: "rgba(100, 210, 180, 0.85)", fontSize: 10 },
    });
  }

  // Artifact-file nodes (U3 + S3 inference). Each file_inputs entry of a file-reading task is either an
  // upstream task OUTPUT (already drawn as a dep edge — no file node) or an EXTERNAL file in the
  // workflow folder (→ a file node + a file edge). They're told apart by pure path arithmetic: an
  // entry equal to deriveUpstreamOutputPaths(dep, task) for some dep is an upstream output; anything
  // else that resolves INSIDE workflows/<wfId>/ is an external file node. Entries that resolve outside
  // the workflow folder (queue paths, outside-tree) stay plain stored file_inputs. Persisted
  // editor_layout `file:` markers also resurface (incl. unwired nodes the user placed). This is the
  // inverse of the U1 save-time derivation (fileNodeInputPath), so the round-trip is lossless.
  const tasksById = Object.fromEntries(taskEntries) as Record<string, JcwfTask>;
  const wfId = workflowId ?? (typeof jcwf.id === "string" ? jcwf.id : "");
  const fileNodeRelPaths = new Set<string>();
  const fileEdgesToAdd: EditorTaskEdge[] = [];

  for (const [taskId, taskValue] of taskEntries)
  {
    const task = taskValue as JcwfTask;
    if (!readsFileInputs(task.type)) continue;
    const fileInputs = Array.isArray(task.file_inputs) ? task.file_inputs as string[] : [];
    const consumerWd = (task.working_directory ?? "") as string;
    const deps = Array.isArray(task.depends_on) ? task.depends_on.filter((d) => taskIdSet.has(d)) : [];

    // Every output path reachable from this task's EXPLICIT deps — those file_inputs are dep-edge wired
    // by the depends_on loop above.
    const depOutputs = new Set<string>();
    for (const depId of deps)
    {
      const depTask = tasksById[depId];
      if (depTask) for (const p of deriveUpstreamOutputPaths(depTask, task, wfId, undefined)) depOutputs.add(p);
    }

    // Map each OTHER task's outputs (expressed relative to THIS consumer) → producer task id. A
    // file_input matching one is an intermediate build artifact produced upstream — even when the
    // workflow declares no depends_on and is wired purely by filename (a Makefile-style graph). Such an
    // input is drawn as a task→task dependency edge (which serialises to depends_on on save), not a
    // floating file node. First producer wins on a duplicate output name.
    const outputToProducer = new Map<string, string>();
    for (const [otherId, otherValue] of taskEntries)
    {
      if (otherId === taskId) continue;
      const otherTask = otherValue as JcwfTask;
      for (const p of deriveUpstreamOutputPaths(otherTask, task, wfId, undefined))
      {
        if (!outputToProducer.has(p)) outputToProducer.set(p, otherId);
      }
    }

    fileInputs.forEach((fi, idx) => {
      const trimmed = fi.trim();
      if (trimmed.length === 0) return;
      if (depOutputs.has(trimmed)) return; // upstream output via explicit dep — dep edge already drawn

      // Implicit upstream output (no explicit depends_on): wire producer → consumer as a dependency edge
      // so the intermediate shows as a connection between nodes instead of a free-floating file node.
      const producerId = outputToProducer.get(trimmed);
      if (producerId)
      {
        const producer = tasksById[producerId];
        const prodOutputs = producer && Array.isArray(producer.file_outputs) ? producer.file_outputs as string[] : [];
        const targetBase = trimmed.split("/").pop() ?? "";
        const matchIdx = prodOutputs.findIndex((o) => (o.split("/").pop() ?? "") === targetBase);
        const sourceHandle = prodOutputs.length <= 1 ? "fileoutput-0" : `fileoutput-${matchIdx >= 0 ? matchIdx : 0}`;
        fileEdgesToAdd.push({
          id: `dep:${producerId}->${taskId}:${idx}`,
          source: producerId,
          target: taskId,
          sourceHandle,
          targetHandle: `dephandle-${idx}`,
          style: { stroke: FILE_INPUT_COLORS[idx % FILE_INPUT_COLORS.length], strokeWidth: 2 },
        });
        return; // intermediate artifact — no file node
      }

      // External input. Resolve through the same base-leaf-strip the runtime uses; only files INSIDE
      // the workflow folder become nodes.
      const fileAbs = normalizePathSegments(`${resolveTaskDirSegments(wfId, consumerWd).join("/")}/${trimmed}`);
      const prefix = ["workflows", wfId];
      if (!prefix.every((seg, i) => fileAbs[i] === seg)) return; // outside the workflow folder
      const relPath = fileAbs.slice(prefix.length).join("/");
      if (relPath.length === 0) return;
      fileNodeRelPaths.add(relPath);
      fileEdgesToAdd.push({
        id: `file:${relPath}->${taskId}:${idx}`,
        source: `file:${relPath}`,
        target: taskId,
        targetHandle: `dephandle-${idx}`,
        style: { stroke: FILE_INPUT_COLORS[idx % FILE_INPUT_COLORS.length], strokeWidth: 2, strokeDasharray: "1 0" },
      });
    });
  }

  // Persisted markers — keep file nodes the user placed (incl. unwired ones) at their saved positions.
  for (const k of Object.keys(savedLayout ?? {}))
  {
    if (k.startsWith("file:")) fileNodeRelPaths.add(k.slice("file:".length));
  }

  // Place inferred (marker-less) file nodes in a column to the left of the leftmost task / filter.
  const fileColumnX = (sortedLevels.length > 0 ? (sortedLevels[0] * cellW) : 0) - cellW * 2;
  let fileRow = 0;
  const fileNodes: EditorFileNode[] = Array.from(fileNodeRelPaths).sort().map((relPath) => {
    const fileNodeId = `file:${relPath}`;
    const saved = savedLayout?.[fileNodeId];
    const position = saved ? { x: saved.x, y: saved.y } : { x: fileColumnX, y: (fileRow++) * (BASE_NODE_HEIGHT + VERTICAL_GAP) };
    return {
      id: fileNodeId,
      type: "file" as const,
      position,
      data: { workflowRelPath: relPath, title: relPath.split("/").pop() || relPath },
    };
  });

  for (const e of fileEdgesToAdd) edges.push(e);

  return { nodes: [...nodes, ...fileNodes], edges };
}
