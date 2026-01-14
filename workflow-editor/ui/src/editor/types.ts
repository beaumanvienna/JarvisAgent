import type { Edge, Node } from "reactflow";

export type EditorTaskNodeData = {
  title: string;      // display label
  subtitle?: string;  // typically task type
};

export type EditorTaskNode = Node<EditorTaskNodeData>;
export type EditorTaskEdge = Edge;

export type EditorGraph = {
  nodes: EditorTaskNode[];
  edges: EditorTaskEdge[];
};
