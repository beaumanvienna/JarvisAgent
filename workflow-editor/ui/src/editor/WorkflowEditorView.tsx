import React, { useCallback, useMemo, useState } from "react";
import ReactFlow, {
  Background,
  Controls,
  MiniMap,
  addEdge,
  type Connection,
  type Edge,
  type Node,
  useEdgesState,
  useNodesState
} from "reactflow";
import "reactflow/dist/style.css";

type SelectedInfo =
  | { kind: "none" }
  | { kind: "node"; node: Node }
  | { kind: "edge"; edge: Edge };

const initialNodes: Node[] = [
  {
    id: "ai_hello",
    position: { x: 80, y: 80 },
    data: { label: "ai_call: hello" },
    type: "default"
  },
  {
    id: "py_format",
    position: { x: 420, y: 80 },
    data: { label: "python: format output" },
    type: "default"
  },
  {
    id: "sh_zip",
    position: { x: 760, y: 80 },
    data: { label: "shell: zip results" },
    type: "default"
  }
];

const initialEdges: Edge[] = [
  { id: "e1", source: "ai_hello", target: "py_format" },
  { id: "e2", source: "py_format", target: "sh_zip" }
];

export default function WorkflowEditorView(): JSX.Element {
  const [nodes, setNodes, onNodesChange] = useNodesState(initialNodes);
  const [edges, setEdges, onEdgesChange] = useEdgesState(initialEdges);
  const [selection, setSelection] = useState<SelectedInfo>({ kind: "none" });

  const onConnect = useCallback(
    (connection: Connection) => {
      setEdges((currentEdges) => {
        return addEdge(connection, currentEdges);
      });
    },
    [setEdges]
  );

  const selectedLabel = useMemo(() => {
    if (selection.kind === "node") {
      return `Node: ${selection.node.id}`;
    }
    if (selection.kind === "edge") {
      return `Edge: ${selection.edge.id}`;
    }
    return "Nothing selected";
  }, [selection]);

  return (
    <div className="editorShell">
      <aside className="sidebar">
        <div className="card">
          <div className="small">Node Palette (placeholder)</div>
          <p className="muted">
            Next: add buttons to insert ai_call / python / shell / internal task
            nodes.
          </p>
        </div>

        <div className="card">
          <div className="small">Tips</div>
          <ul className="muted" style={{ marginTop: 8 }}>
            <li>Drag nodes, connect edges</li>
            <li>Hold space / scroll to pan/zoom</li>
            <li>Minimap + controls are enabled</li>
          </ul>
        </div>
      </aside>

      <div style={{ height: "100%", width: "100%" }}>
        <ReactFlow
          nodes={nodes}
          edges={edges}
          onNodesChange={onNodesChange}
          onEdgesChange={onEdgesChange}
          onConnect={onConnect}
          onNodeClick={(_, node) => { setSelection({ kind: "node", node }); }}
          onEdgeClick={(_, edge) => { setSelection({ kind: "edge", edge }); }}
          onPaneClick={() => { setSelection({ kind: "none" }); }}
          fitView
        >
          <Background />
          <MiniMap />
          <Controls />
        </ReactFlow>
      </div>

      <aside className="inspector">
        <div className="card">
          <div className="small">Inspector (placeholder)</div>
          <div style={{ marginTop: 8 }}>{selectedLabel}</div>
          <p className="muted">
            Next: show/edit task properties here (id, type, label, doc,
            working_directory, params).
          </p>
        </div>

        <div className="card">
          <div className="small">Current graph (debug)</div>
          <div className="code">
            nodes={nodes.length}, edges={edges.length}
          </div>
        </div>
      </aside>
    </div>
  );
}
