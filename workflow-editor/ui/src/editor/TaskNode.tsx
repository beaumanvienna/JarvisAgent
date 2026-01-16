import React from "react";
import { Handle, Position, type NodeProps } from "reactflow";
import type { EditorTaskNodeData } from "./types";

export default function TaskNode(props: NodeProps<EditorTaskNodeData>): JSX.Element
{
  const isSelected = props.selected === true;
  const subtitle = props.data.subtitle ?? "";
  const errors = props.data.validationErrors ?? [];
  const warnings = props.data.validationWarnings ?? [];
  const isDirty = props.data.isDirty === true;

  const firstError = errors.length > 0 ? errors[0] : null;
  const firstWarning = warnings.length > 0 ? warnings[0] : null;

  const errorCount = errors.length;
  const warningCount = warnings.length;

  const tooltipLines: string[] = [];
  tooltipLines.push(props.data.title);
  if (subtitle.length > 0)
  {
    tooltipLines.push("");
    tooltipLines.push(`Type: ${subtitle}`);
  }
  if (isDirty)
  {
    tooltipLines.push("");
    tooltipLines.push("Unsaved changes.");
  }
  if (errorCount > 0)
  {
    tooltipLines.push("");
    tooltipLines.push(`${errorCount} error(s):`);
    for (const e of errors)
    {
      tooltipLines.push(`- ${e}`);
    }
  }
  if (warningCount > 0)
  {
    tooltipLines.push("");
    tooltipLines.push(`${warningCount} warning(s):`);
    for (const w of warnings)
    {
      tooltipLines.push(`- ${w}`);
    }
  }

  const tooltip = tooltipLines.join("\n");

  return (
    <div
      className={
        "taskNode"
        + (isSelected ? " taskNodeSelected" : "")
        + (firstError ? " taskNodeError" : "")
        + (!firstError && firstWarning ? " taskNodeWarning" : "")
        + (isDirty ? " taskNodeDirty" : "")
      }
      title={tooltip}
    >
      <Handle type="target" position={Position.Left} />
      <div className="taskNodeBody">
        <div className="taskNodeTitle">
          {props.data.title}
          <span className="taskNodeBadges">
            {isDirty ? <span className="taskNodeBadge taskNodeBadgeDirty">*</span> : null}
            {errorCount > 0 ? <span className="taskNodeBadge taskNodeBadgeError">E{errorCount}</span> : null}
            {warningCount > 0 ? <span className="taskNodeBadge taskNodeBadgeWarning">W{warningCount}</span> : null}
          </span>
        </div>
        {subtitle.length > 0 ? <div className="taskNodeSubtitle">{subtitle}</div> : null}
        {firstError ? <div className="taskNodeErrorText">{firstError}{errorCount > 1 ? ` (+${errorCount - 1})` : ""}</div> : null}
        {!firstError && firstWarning ? <div className="taskNodeWarningText">{firstWarning}{warningCount > 1 ? ` (+${warningCount - 1})` : ""}</div> : null}
      </div>
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
