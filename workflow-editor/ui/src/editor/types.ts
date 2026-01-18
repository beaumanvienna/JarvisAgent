import type { Edge, Node } from "reactflow";
import type { JcwfTask } from "../jcwf/types";

export type RuntimeTaskState = "queued" | "running" | "success" | "failed" | "cancelled" | "unknown";

export type EditorTaskNodeData = {
  task: JcwfTask;
  title: string;
  subtitle?: string;
  validationErrors?: string[];
  validationWarnings?: string[];
  isDirty?: boolean;

  // Live run monitoring (populated from WebSocket snapshots)
  runtimeState?: RuntimeTaskState;
  runtimeRunId?: string;
};

export type EditorTaskNode = Node<EditorTaskNodeData> & { type: "task" };
export type EditorTaskEdge = Edge;

export type EditorGraph = {
  nodes: EditorTaskNode[];
  edges: EditorTaskEdge[];
};

export type ValidationResult = {
  nodeErrorsById: Map<string, string[]>;
  nodeWarningsById?: Map<string, string[]>;
  cycleNodes: string[];
};
