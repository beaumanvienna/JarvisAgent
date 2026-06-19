import type { Edge, Node } from "reactflow";
import type { JcwfControlNode, JcwfFilter, JcwfTask } from "../jcwf/types";

export type RuntimeTaskState = "queued" | "running" | "success" | "fresh" | "failed" | "cancelled" | "unknown";

export type EditorTaskNodeData = {
  task: JcwfTask;
  title: string;
  subtitle?: string;
  validationErrors?: string[];
  validationWarnings?: string[];
  validationInfos?: string[];
  isDirty?: boolean;
  hideTierDWarnings?: boolean;

  // Live run monitoring (populated from WebSocket snapshots)
  runtimeState?: RuntimeTaskState;
  runtimeRunId?: string;
  runtimeError?: string;
  capturedStdout?: string;
  capturedStderr?: string;
  isRunPaused?: boolean;
};

export type EditorFilterNodeData = {
  filter: JcwfFilter;
  title: string;
  subtitle?: string;
  runtimeItemCount?: number;
  runtimeProgress?: string;
};

export type EditorControlNodeData = {
  controlNode: JcwfControlNode;
  title: string;
  subtitle?: string;
};

// Artifact-file node (U3) — a pure editor overlay, NOT a JCWF task. It represents an input file that
// lives in the workflow folder; wiring it into a task's input port is what produces that task's
// file_inputs path. graphToJcwf emits no task for it (only an editor_layout position); jcwfToGraph
// reconstructs it. `workflowRelPath` is the file's path relative to workflows/<id>/ and is the node's
// stable identity (id = `file:<workflowRelPath>`).
export type EditorFileNodeData = {
  workflowRelPath: string;
  title: string; // display basename
};

export type EditorTaskNode = Node<EditorTaskNodeData> & { type: "task" };
export type EditorFilterNode = Node<EditorFilterNodeData> & { type: "filter" };
export type EditorControlNode = Node<EditorControlNodeData> & { type: "branch" };
export type EditorFileNode = Node<EditorFileNodeData> & { type: "file" };
export type EditorNode = EditorTaskNode | EditorFilterNode | EditorControlNode | EditorFileNode;
export type EditorTaskEdge = Edge;

// Narrowing guard for node-array iterations. Node arrays are frequently typed/cast as
// `EditorTaskNode[]` even though at runtime they also hold filter and branch nodes — for those
// `n.data.task` is `undefined`, so reading any field off it throws. Always guard with `isTaskNode(n)`
// (or an inline `n.type === "task"`) before touching `n.data.task` inside a `.map`/`.filter`/`for`
// over a node array. A missing guard here threw on a CSV-filter node mid-keystroke, aborting the
// state commit and making inspector path fields look uneditable.
export function isTaskNode(n: EditorNode): n is EditorTaskNode
{
  return n.type === "task";
}

// Sibling guard for the artifact-file overlay nodes (id `file:<relpath>`). Same discipline as
// isTaskNode: the node arrays are typed broadly, so guard before reading `.data.workflowRelPath`.
export function isFileNode(n: EditorNode): n is EditorFileNode
{
  return n.type === "file";
}

export type EditorGraph = {
  nodes: EditorNode[];
  edges: EditorTaskEdge[];
};

export type ValidationResult = {
  nodeErrorsById: Map<string, string[]>;
  nodeWarningsById?: Map<string, string[]>;
  nodeInfosById?: Map<string, string[]>;
  cycleNodes: string[];
};
