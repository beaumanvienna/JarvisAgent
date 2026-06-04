import React from "react";
import { Handle, Position, type NodeProps } from "reactflow";
import type { EditorControlNodeData } from "./types";

export default function BranchNode(props: NodeProps<EditorControlNodeData>): JSX.Element
{
  const isSelected = props.selected === true;
  const title = props.data.title;
  const subtitle = props.data.subtitle ?? "branch";

  const tooltipLines: string[] = [];
  tooltipLines.push(title);
  tooltipLines.push("");
  tooltipLines.push(`Type: ${subtitle}`);
  const tooltip = tooltipLines.join("\n");

  return (
    <div
      className={
        "branchNode"
        + (isSelected ? " branchNodeSelected" : "")
      }
      title={tooltip}
    >
      <Handle type="target" position={Position.Left} id="cf-in-normal" title="normal in" style={{ top: "35%" }} />
      <Handle type="target" position={Position.Left} id="cf-in-error" title="error signal in" style={{ top: "65%" }} />

      <div className="branchNodeBody">
        <div className="branchNodeTitle">{title}</div>
        <div className="branchNodeSubtitle">{subtitle}</div>
      </div>

      <Handle type="source" position={Position.Right} id="cf-out-normal" title="normal out" style={{ top: "35%" }} />
      <Handle type="source" position={Position.Right} id="cf-out-error" title="on_error out" style={{ top: "65%" }} />
    </div>
  );
}
