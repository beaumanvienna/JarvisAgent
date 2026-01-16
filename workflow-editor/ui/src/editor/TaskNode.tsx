import React from "react";
import { Handle, Position, type NodeProps } from "reactflow";
import type { EditorTaskNodeData } from "./types";

export default function TaskNode(props: NodeProps<EditorTaskNodeData>): JSX.Element
{
  const isSelected = props.selected === true;
  const subtitle = props.data.subtitle ?? "";
  const firstError = props.data.validationErrors && props.data.validationErrors.length > 0
    ? props.data.validationErrors[0]
    : null;

  const tooltip = firstError
    ? `${props.data.title}\n\nError: ${firstError}`
    : (subtitle.length > 0 ? `${subtitle}: ${props.data.title}` : props.data.title);

  return (
    <div
      className={"taskNode" + (isSelected ? " taskNodeSelected" : "") + (firstError ? " taskNodeError" : "")}
      title={tooltip}
    >
      <Handle type="target" position={Position.Left} />
      <div className="taskNodeBody">
        <div className="taskNodeTitle">{props.data.title}</div>
        {subtitle.length > 0 ? <div className="taskNodeSubtitle">{subtitle}</div> : null}
        {firstError ? <div className="taskNodeErrorText">{firstError}</div> : null}
      </div>
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
