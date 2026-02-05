import React from "react";
import { Handle, Position, type NodeProps } from "reactflow";
import type { EditorTaskNodeData, RuntimeTaskState } from "./types";

function runtimeBadgeLabel(runtimeState: RuntimeTaskState | undefined): { label: string; className: string } | null
{
  if (!runtimeState)
  {
    return null;
  }

  switch (runtimeState)
  {
    case "queued":
      return { label: "Q", className: "taskNodeBadgeQueued" };
    case "running":
      return { label: "R", className: "taskNodeBadgeRunning" };
    case "success":
      return { label: "OK", className: "taskNodeBadgeSuccess" };
    case "failed":
      return { label: "F", className: "taskNodeBadgeFailed" };
    case "cancelled":
      return { label: "C", className: "taskNodeBadgeCancelled" };
    default:
      return { label: "?", className: "taskNodeBadgeUnknown" };
  }
}


export default function TaskNode(props: NodeProps<EditorTaskNodeData>): JSX.Element
{
  const isSelected = props.selected === true;
  const subtitle = props.data.subtitle ?? "";
  const errors = props.data.validationErrors ?? [];
  const warnings = props.data.validationWarnings ?? [];
  const infos = props.data.hideTierDWarnings ? [] : (props.data.validationInfos ?? []);
  const runtimeState = props.data.runtimeState;

  const isDirty = props.data.isDirty === true;

  const firstError = errors.length > 0 ? errors[0] : null;
  const firstWarning = warnings.length > 0 ? warnings[0] : null;
  const firstInfo = infos.length > 0 ? infos[0] : null;

  const runtimeBadge = runtimeBadgeLabel(runtimeState);

  const errorCount = errors.length;
  const warningCount = warnings.length;
  const infoCount = infos.length;

  const tooltipLines: string[] = [];
  tooltipLines.push(props.data.title);
  if (subtitle.length > 0)
  {
    tooltipLines.push("");
    tooltipLines.push(`Type: ${subtitle}`);
  }
  if (runtimeState)
  {
    tooltipLines.push("");
    tooltipLines.push(`Run state: ${runtimeState}`);
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

  if (infoCount > 0)
  {
    tooltipLines.push("");
    tooltipLines.push(`${infoCount} info:`);
    for (const i of infos)
    {
      tooltipLines.push(`- ${i}`);
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
        + (!firstError && !firstWarning && firstInfo ? " taskNodeInfo" : "")
        + (runtimeState === "running" ? " taskNodeRunning" : "")
        + (runtimeState === "success" ? " taskNodeSuccess" : "")
        + (runtimeState === "failed" ? " taskNodeFailed" : "")
        + (runtimeState === "cancelled" ? " taskNodeCancelled" : "")
        + (isDirty ? " taskNodeDirty" : "")
      }
      title={tooltip}
    >
      <Handle type="target" position={Position.Left} />
      <div className="taskNodeBody">
        <div className="taskNodeTitle">
          {props.data.title}
          <span className="taskNodeBadges">
            {runtimeBadge ? (<span className={`taskNodeBadge ${runtimeBadge.className}`}>{runtimeBadge.label}</span>) : null}
            {isDirty ? <span className="taskNodeBadge taskNodeBadgeDirty">*</span> : null}
            {errorCount > 0 ? <span className="taskNodeBadge taskNodeBadgeError">E{errorCount}</span> : null}
            {warningCount > 0 ? <span className="taskNodeBadge taskNodeBadgeWarning">W{warningCount}</span> : null}
            {infoCount > 0 ? <span className="taskNodeBadge taskNodeBadgeInfo">I{infoCount}</span> : null}
          </span>
        </div>
        {subtitle.length > 0 ? <div className="taskNodeSubtitle">{subtitle}</div> : null}
        {firstError ? <div className="taskNodeErrorText">{firstError}{errorCount > 1 ? ` (+${errorCount - 1})` : ""}</div> : null}
        {!firstError && firstWarning ? <div className="taskNodeWarningText">{firstWarning}{warningCount > 1 ? ` (+${warningCount - 1})` : ""}</div> : null}
        {!firstError && !firstWarning && firstInfo ? <div className="taskNodeInfoText">{firstInfo}{infoCount > 1 ? ` (+${infoCount - 1})` : ""}</div> : null}
      </div>
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
