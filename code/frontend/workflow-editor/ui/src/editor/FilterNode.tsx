import React from "react";
import { Handle, Position, type NodeProps } from "reactflow";
import type { EditorFilterNodeData } from "./types";

export default function FilterNode(props: NodeProps<EditorFilterNodeData>): JSX.Element
{
  const isSelected = props.selected === true;
  const subtitle = props.data.subtitle ?? "";
  const runtimeItemCount = props.data.runtimeItemCount;
  const runtimeProgress = props.data.runtimeProgress;

  const tooltipLines: string[] = [];
  tooltipLines.push(props.data.title);
  if (subtitle.length > 0)
  {
    tooltipLines.push(`Source: ${subtitle}`);
  }
  if (runtimeItemCount !== undefined)
  {
    tooltipLines.push(`Items: ${runtimeItemCount}`);
  }
  if (runtimeProgress)
  {
    tooltipLines.push(`Progress: ${runtimeProgress}`);
  }

  const tooltip = tooltipLines.join("\n");

  return (
    <div
      className={
        "filterNode"
        + (isSelected ? " filterNodeSelected" : "")
      }
      title={tooltip}
    >
      <Handle type="target" position={Position.Left} />
      <div className="filterNodeBody">
        <div className="filterNodeIcon">&#9881;</div>
        <div className="filterNodeContent">
          <div className="filterNodeTitle">{props.data.title}</div>
          {subtitle.length > 0 ? <div className="filterNodeSubtitle">{subtitle}</div> : null}
          {runtimeItemCount !== undefined
            ? <div className="filterNodeItemCount">{runtimeItemCount} items</div>
            : null}
        </div>
      </div>
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
