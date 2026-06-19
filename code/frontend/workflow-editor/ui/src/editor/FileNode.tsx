import React from "react";
import { Handle, Position, type NodeProps } from "reactflow";
import type { EditorFileNodeData } from "./types";

// Artifact-file node (U3) — a small "📄 <name>" chip with a single output port. It is a pure editor
// overlay: wiring its output into a task's input port is what gives that task its file_inputs path.
// Uses inline styles (no CSS-class dependency) so it renders self-contained.
export default function FileNode(props: NodeProps<EditorFileNodeData>): JSX.Element
{
  const isSelected = props.selected === true;
  const { workflowRelPath, title } = props.data;

  return (
    <div
      title={workflowRelPath}
      style={{
        display: "flex",
        alignItems: "center",
        gap: 6,
        padding: "6px 10px",
        borderRadius: 6,
        fontSize: 12,
        background: "rgba(80, 120, 160, 0.18)",
        border: `1px solid ${isSelected ? "rgba(140, 200, 255, 0.95)" : "rgba(120, 170, 210, 0.5)"}`,
        color: "rgba(220, 235, 250, 0.95)",
        maxWidth: 220,
      }}
    >
      <span style={{ flexShrink: 0 }}>&#128196;</span>
      <span style={{ overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{title}</span>
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
