import type { EditorGraph, EditorTaskEdge, EditorTaskNode } from "./types";
import type { JcwfFile, JcwfTask } from "../jcwf/types";

function displayTitle(task: JcwfTask): { title: string; subtitle?: string }
{
  return {
    title: task.label && task.label.length > 0 ? task.label : task.id,
    subtitle: task.type,
  };
}

export function jcwfToGraph(jcwf: JcwfFile): EditorGraph
{
  const taskEntries = Object.entries(jcwf.tasks ?? {});
  const colCount = 4;
  const cellW = 260;
  const cellH = 140;

  const nodes: EditorTaskNode[] = taskEntries.map(([taskId, taskValue], index) => {
    const task = taskValue as JcwfTask;
    const { title, subtitle } = displayTitle(task);
    const col = index % colCount;
    const row = Math.floor(index / colCount);

    return {
      id: taskId,
      type: "task",
      position: { x: col * cellW, y: row * cellH },
      data: { task, title, subtitle },
    };
  });

  const edges: EditorTaskEdge[] = [];
  for (const [taskId, taskValue] of taskEntries)
  {
    const task = taskValue as JcwfTask;
    const deps = Array.isArray(task.depends_on) ? task.depends_on : [];
    for (const depId of deps)
    {
      edges.push({
        id: `dep:${depId}->${taskId}`,
        source: depId,
        target: taskId,
      });
    }
  }

  return { nodes, edges };
}
