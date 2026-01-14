import React, { useCallback, useMemo, useState } from "react";
import ReactFlow, {
  Background,
  Controls,
  MiniMap,
  addEdge,
  type Connection,
  useEdgesState,
  useNodesState
} from "reactflow";
import "reactflow/dist/style.css";

import TaskNode from "./TaskNode";

import type { EditorTaskNode, EditorTaskEdge } from "./types";
import { jcwfToGraph } from "./jcwfToGraph";
import { graphToJcwf } from "./graphToJcwf";
import type { JcwfWorkflow } from "../jcwf/types";

const nodeTypes = { task: TaskNode };

const sampleWorkflow: JcwfWorkflow = {
  version: "1.0",
  id: "sample-editor-workflow",
  label: "Sample Editor Workflow",
  doc: "Hard-coded sample until the backend CRUD endpoints are wired into the UI.",
  tasks: {
    ai_task: {
      id: "ai_task",
      type: "ai_call",
      label: "AI Task",
      depends_on: []
    },
    python_task: {
      id: "python_task",
      type: "python",
      label: "Python Task",
      depends_on: ["ai_task"]
    },
    shell_task: {
      id: "shell_task",
      type: "shell",
      label: "Shell Task",
      depends_on: ["python_task"]
    }
  }
};

export default function WorkflowEditorView(): JSX.Element
{
  const initialGraph = useMemo(() => {
    return jcwfToGraph(sampleWorkflow);
  }, []);

  const [nodes, setNodes, onNodesChange] = useNodesState<EditorTaskNode>(initialGraph.nodes);
  const [edges, setEdges, onEdgesChange] = useEdgesState<EditorTaskEdge>(initialGraph.edges);
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);
  const [lastExportError, setLastExportError] = useState<string>("");

  const onConnect = useCallback(
    (connection: Connection) =>
    {
      setEdges((currentEdges) => addEdge({ ...connection, type: "smoothstep" }, currentEdges));
    },
    [setEdges]
  );

  const onSelectionChange = useCallback(
    (params: { nodes: EditorTaskNode[] }) =>
    {
      if (params.nodes.length > 0)
      {
        setSelectedNodeId(params.nodes[0].id);
      }
      else
      {
        setSelectedNodeId(null);
      }
    },
    []
  );

  const onExportJcwf = useCallback(() =>
  {
    try
    {
      setLastExportError("");
      const jcwf = graphToJcwf({ nodes, edges }, sampleWorkflow.id);
      // eslint-disable-next-line no-console
      console.log("=== Exported JCWF ===");
      // eslint-disable-next-line no-console
      console.log(JSON.stringify(jcwf, null, 2));
    }
    catch (err)
    {
      const message = err instanceof Error ? err.message : String(err);
      setLastExportError(message);
    }
  }, [edges, nodes]);

  const selectedNode = useMemo(() =>
  {
    if (selectedNodeId === null)
    {
      return null;
    }
    return nodes.find((n) => n.id === selectedNodeId) ?? null;
  }, [nodes, selectedNodeId]);

  return (
    <div className="editorLayout">
      <div className="editorCanvas">
        <ReactFlow
          nodes={nodes}
          edges={edges}
          onNodesChange={onNodesChange}
          onEdgesChange={onEdgesChange}
          onConnect={onConnect}
          onSelectionChange={onSelectionChange}
          nodeTypes={nodeTypes}
          fitView
        >
          <Background />
          <Controls />
          <MiniMap />
        </ReactFlow>
      </div>

      <aside className="editorSidebar">
        <div className="card">
          <div className="small">Inspector</div>
          {selectedNode ? (
            <div className="code">
              <div>id: {selectedNode.id}</div>
              <div>type: {(selectedNode.data as any)?.subtitle ?? ""}</div>
              <div>label: {(selectedNode.data as any)?.title ?? ""}</div>
            </div>
          ) : (
            <p className="muted">Select a node to inspect it.</p>
          )}
        </div>

        <div className="card">
          <div className="small">JCWF</div>
          <button className="btn" onClick={onExportJcwf} type="button">
            Export JCWF to console
          </button>
          {lastExportError.length > 0 ? (
            <p className="muted" style={{ marginTop: 8 }}>
              Export failed: {lastExportError}
            </p>
          ) : null}
        </div>

        <div className="card">
          <div className="small">Current graph (debug)</div>
          <div className="code">nodes={nodes.length}, edges={edges.length}</div>
        </div>
      </aside>
    </div>
  );
}