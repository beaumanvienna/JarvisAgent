import React, { useCallback, useMemo } from "react";
import ReactFlow, {
  Background,
  Controls,
  MiniMap,
  Position,
  useEdgesState,
  useNodesState,
  type Node,
  type Edge,
} from "reactflow";

import "reactflow/dist/style.css";

import TaskNode from "./TaskNode";
import { jcwfToGraph } from "./jcwfToGraph";
import { graphToJcwf } from "./graphToJcwf";
import type {
  EditorGraph,
  EditorTaskNode,
  EditorTaskNodeData,
  EditorTaskEdge,
} from "./types";

const sampleWorkflowUrl = new URL("../../../../example/workflows/aiCarMaintenancePipeline.jcwf", import.meta.url);


const nodeTypes = {
  task: TaskNode,
};

export default function WorkflowEditorView(): JSX.Element {
  // Build initial graph from sample JCWF
  const initialGraph: EditorGraph = useMemo(() => {
    return { nodes: [], edges: [] };
  }, []);


  /**
   * IMPORTANT:
   * useNodesState<T> expects T = node.data type,
   * NOT the full Node type.
   */
  const [nodes, setNodes, onNodesChange] =
    useNodesState<EditorTaskNodeData>(
      initialGraph.nodes as Node<EditorTaskNodeData>[]
    );

  const [edges, setEdges, onEdgesChange] =
    useEdgesState<EditorTaskEdge>(initialGraph.edges);

  const onExportJcwf = useCallback(() => {
    const graph: EditorGraph = {
      nodes: nodes as EditorTaskNode[],
      edges,
    };


    const jcwf = graphToJcwf(graph, "aiCarMaintenancePipeline");

    // eslint-disable-next-line no-console
    console.log("Exported JCWF:", jcwf);
  }, [nodes, edges]);

  return (
    <div style={{ width: "100%", height: "100%" }}>
      <div style={{ padding: "6px", borderBottom: "1px solid #333" }}>
        <button onClick={onExportJcwf}>Export JCWF (console)</button>
      </div>

      <ReactFlow
        nodes={nodes}
        edges={edges}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        nodeTypes={nodeTypes}
        fitView
      >
        <Background />
        <Controls />
        <MiniMap />
      </ReactFlow>
    </div>
  );
}
