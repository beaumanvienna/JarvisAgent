import type { EditorGraph, EditorFilterNode, EditorTaskEdge, EditorTaskNode, EditorNode } from "./types";
import type { JcwfDataflowEntry, JcwfFile, JcwfFilter, JcwfTask } from "../jcwf/types";
import { FILE_INPUT_COLORS } from "./constants";

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

export function jcwfToGraph(jcwf: JcwfFile): EditorGraph
{
  const taskEntries = Object.entries(jcwf.tasks ?? {});
  const taskIdSet = new Set<string>(taskEntries.map(([taskId]) => taskId));
  const cellW = 320;
  const VERTICAL_GAP = 30;
  const BASE_NODE_HEIGHT = 60;
  const DEP_ROW_HEIGHT = 15;

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
    const position = positionByTaskId.get(taskId) ?? { x: 0, y: 0 };
    return {
      id: taskId,
      type: "task" as const,
      position,
      data: { task, title, subtitle },
    };
  });

  // Filter nodes — placed to the left of the leftmost task column
  const filters = Array.isArray(jcwf.filters) ? jcwf.filters : [];
  const filterNodes: EditorFilterNode[] = filters.map((filter, index) => {
    const { title, subtitle } = filterDisplayTitle(filter);
    const minX = sortedLevels.length > 0 ? (sortedLevels[0] * cellW) : 0;
    return {
      id: `filter:${filter.id}`,
      type: "filter" as const,
      position: { x: minX - cellW, y: index * (BASE_NODE_HEIGHT + VERTICAL_GAP) },
      data: { filter, title, subtitle },
    };
  });

  const nodes: EditorNode[] = [...filterNodes, ...taskNodes];

  const edges: EditorTaskEdge[] = [];
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

  return { nodes, edges };
}
