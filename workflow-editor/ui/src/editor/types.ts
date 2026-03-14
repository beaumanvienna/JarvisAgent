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

export type EditorTaskNode = Node<EditorTaskNodeData> & { type: "task" };
export type EditorFilterNode = Node<EditorFilterNodeData> & { type: "filter" };
export type EditorControlNode = Node<EditorControlNodeData> & { type: "branch" };
export type EditorNode = EditorTaskNode | EditorFilterNode | EditorControlNode;
export type EditorTaskEdge = Edge;

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
