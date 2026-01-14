import React from "react";
import { Handle, Position, type NodeProps } from "reactflow";

export type TaskNodeData = {
  title: string;
  subtitle?: string;
};

export default function TaskNode(props: NodeProps<TaskNodeData>): JSX.Element
{
  const isSelected = props.selected === true;
  const subtitle = props.data.subtitle ?? "";

  return (
    <div
      className={"taskNode" + (isSelected ? " taskNodeSelected" : "")}
      title={props.data.subtitle ? `${props.data.subtitle}: ${props.data.title}` : props.data.title}
    >
      <Handle type="target" position={Position.Left} />
      <div className="taskNodeBody">
        <div className="taskNodeTitle">{props.data.title}</div>
        {subtitle.length > 0 ? <div className="taskNodeSubtitle">{subtitle}</div> : null}
      </div>
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
