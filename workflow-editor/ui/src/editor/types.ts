import type { Edge, Node } from "reactflow";
import type { JcwfTask } from "../jcwf/types";

export type EditorTaskNodeData = {
  task: JcwfTask;
  title: string;
  subtitle?: string;
  validationErrors?: string[];
};

export type EditorTaskNode = Node<EditorTaskNodeData> & { type: "task" };
export type EditorTaskEdge = Edge;

export type EditorGraph = {
  nodes: EditorTaskNode[];
  edges: EditorTaskEdge[];
};

export type ValidationResult = {
  nodeErrorsById: Map<string, string[]>;
  cycleNodes: string[];
};
