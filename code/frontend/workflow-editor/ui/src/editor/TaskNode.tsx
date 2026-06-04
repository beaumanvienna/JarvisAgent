import React from "react";
import { Handle, Position, type NodeProps } from "reactflow";
import type { EditorTaskNodeData, RuntimeTaskState } from "./types";
import { FILE_INPUT_COLORS, FILE_OUTPUT_COLORS } from "./constants";

function runtimeBadgeLabel(runtimeState: RuntimeTaskState | undefined, isRunPaused?: boolean): { label: string; className: string } | null
{
  if (!runtimeState)
  {
    return null;
  }

  switch (runtimeState)
  {
    case "queued":
      return isRunPaused
        ? { label: "❚❚", className: "taskNodeBadgePaused" }
        : { label: "Q", className: "taskNodeBadgeQueued" };
    case "running":
      return { label: "R", className: "taskNodeBadgeRunning" };
    case "success":
      return { label: "OK", className: "taskNodeBadgeSuccess" };
    case "failed":
      return { label: "F", className: "taskNodeBadgeFailed" };
    case "fresh":
      return { label: "✓", className: "taskNodeBadgeFresh" };
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

  const capturedStdout = props.data.capturedStdout ?? "";
  const capturedStderr = props.data.capturedStderr ?? "";
  const hasOutput = capturedStdout.length > 0 || capturedStderr.length > 0;

  const firstError = errors.length > 0 ? errors[0] : null;
  const firstWarning = warnings.length > 0 ? warnings[0] : null;
  const firstInfo = infos.length > 0 ? infos[0] : null;

  const runtimeBadge = runtimeBadgeLabel(runtimeState, props.data.isRunPaused);

  const taskInputs = props.data.task.inputs as Record<string, unknown> | undefined;
  const taskOutputs = props.data.task.outputs as Record<string, unknown> | undefined;
  const inputNames: string[] = taskInputs && typeof taskInputs === "object" ? Object.keys(taskInputs) : [];
  const outputNames: string[] = taskOutputs && typeof taskOutputs === "object" ? Object.keys(taskOutputs) : [];

  const fileInputs: string[] = Array.isArray(props.data.task.file_inputs) ? props.data.task.file_inputs as string[] : [];
  const depIds: string[] = Array.isArray(props.data.task.depends_on) ? props.data.task.depends_on as string[] : [];
  const materializeMap = (props.data.task.materialize ?? {}) as Record<string, string>;

  const isSubWorkflow = props.data.task.type === "sub_workflow";
  const exposeErrorSignal = props.data.task.expose_error_signal === true;

  const fileOutputs: string[] = Array.isArray(props.data.task.file_outputs) ? props.data.task.file_outputs as string[] : [];
  const hasFileOutputs = fileOutputs.length > 0;

  const fileOutputEntries = fileOutputs.map((filePath, idx) => {
    const color = FILE_OUTPUT_COLORS[idx % FILE_OUTPUT_COLORS.length];
    const segments = filePath.split("/");
    const filename = segments[segments.length - 1] || "";
    const label = filename.length > 20 ? filename.slice(0, 18) + "\u2026" : (filename || `Output ${idx + 1}`);
    return { label, color };
  });

  const handleCount = Math.max(fileInputs.length, depIds.length);
  const hasDepHandles = handleCount > 0;

  const handleEntries = Array.from({ length: handleCount }, (_, idx) => {
    const color = FILE_INPUT_COLORS[idx % FILE_INPUT_COLORS.length];
    const isFileInput = idx < fileInputs.length;

    let label: string;
    if (isFileInput) {
      const template = `{{input[${idx}]}}`;
      const matTarget = materializeMap[template];
      if (matTarget) {
        label = matTarget;
      } else {
        const filePath = fileInputs[idx];
        const segments = filePath.split("/");
        const filename = segments[segments.length - 1] || "";
        label = filename.length > 20 ? filename.slice(0, 18) + "\u2026" : (filename || `Input ${idx + 1}`);
      }
    } else if (idx < depIds.length) {
      const depId = depIds[idx];
      label = depId.length > 22 ? depId.slice(0, 20) + "\u2026" : depId;
    } else {
      label = `Input ${idx + 1}`;
    }

    return { label, color, isFileInput };
  });


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
        + (isSubWorkflow ? " taskNodeSubWorkflow" : "")
        + (isSelected ? " taskNodeSelected" : "")
        + (firstError ? " taskNodeError" : "")
        + (!firstError && firstWarning ? " taskNodeWarning" : "")
        + (!firstError && !firstWarning && firstInfo ? " taskNodeInfo" : "")
        + (runtimeState === "queued" ? " taskNodeQueued" : "")
        + (runtimeState === "running" ? " taskNodeRunning" : "")
        + (runtimeState === "success" ? " taskNodeSuccess" : "")
        + (runtimeState === "fresh" ? " taskNodeFresh" : "")
        + (runtimeState === "failed" ? " taskNodeFailed" : "")
        + (runtimeState === "cancelled" ? " taskNodeCancelled" : "")
        + (isDirty ? " taskNodeDirty" : "")
      }
      title={tooltip}
    >
      {hasOutput && (
        <div className="taskNodeOutputPopup">
          {capturedStderr.length > 0 && (
            <pre className="taskNodeOutputStderr">{capturedStderr}</pre>
          )}
          {capturedStdout.length > 0 && (
            <pre className="taskNodeOutputStdout">{capturedStdout}</pre>
          )}
        </div>
      )}
      {!hasDepHandles && (
        <Handle type="target" position={Position.Left} id="dep-target" />
      )}
      {hasDepHandles && (
        <Handle type="target" position={Position.Left} id="dep-target"
          className="hiddenHandle" />
      )}
      {inputNames.map((name, idx) => (
        <Handle
          key={`in:${name}`}
          type="target"
          position={Position.Left}
          id={`in:${name}`}
          style={{ top: `${50 + (idx + 1) * 16}%` }}
          className="dataflowHandle dataflowHandleInput"
          title={`input: ${name}`}
        />
      ))}
      <div className="taskNodeBody">
        <div className="taskNodeTitle">
          {isSubWorkflow && <span className="taskNodeSubWorkflowIcon">{"\u29C9"}</span>}
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
        {isSubWorkflow && (
          <div className="taskNodeSubWorkflowHint">double-click to enter</div>
        )}
        {hasDepHandles && (
          <div className="taskNodeFileInputs">
            {handleEntries.map((entry, idx) => (
              <div key={idx} className="taskNodeDepLabel taskNodeHandleRow">
                <Handle
                  type="target"
                  position={Position.Left}
                  id={`dephandle-${idx}`}
                  className="taskNodeInlineHandle"
                  style={{
                    background: entry.color,
                    borderColor: entry.color,
                    width: 10,
                    height: 10,
                    borderWidth: 2,
                  }}
                  title={`${idx + 1}: ${entry.label}`}
                />
                <span style={{ color: entry.color }}>
                  {idx + 1}: {entry.label}
                </span>
              </div>
            ))}
          </div>
        )}
        {firstError ? <div className="taskNodeErrorText">{firstError}{errorCount > 1 ? ` (+${errorCount - 1})` : ""}</div> : null}
        {!firstError && firstWarning ? <div className="taskNodeWarningText">{firstWarning}{warningCount > 1 ? ` (+${warningCount - 1})` : ""}</div> : null}
        {!firstError && !firstWarning && firstInfo ? <div className="taskNodeInfoText">{firstInfo}{infoCount > 1 ? ` (+${infoCount - 1})` : ""}</div> : null}
        {hasFileOutputs && (
          <div className="taskNodeFileOutputs">
            {fileOutputEntries.map((entry, idx) => (
              <div key={idx} className="taskNodeOutputLabel taskNodeHandleRow">
                <span style={{ color: entry.color }}>
                  out: {entry.label}
                </span>
                <Handle
                  type="source"
                  position={Position.Right}
                  id={`fileoutput-${idx}`}
                  className="taskNodeInlineHandle taskNodeInlineHandleRight"
                  style={{
                    background: entry.color,
                    borderColor: entry.color,
                    width: 10,
                    height: 10,
                    borderWidth: 2,
                  }}
                  title={`Output ${idx + 1}: ${entry.label}`}
                />
              </div>
            ))}
          </div>
        )}
        {(inputNames.length > 0 || outputNames.length > 0) && (
          <div className="taskNodePorts">
            {inputNames.length > 0 && <div className="taskNodePortList"><span className="taskNodePortLabel">in:</span> {inputNames.join(", ")}</div>}
            {outputNames.length > 0 && <div className="taskNodePortList"><span className="taskNodePortLabel">out:</span> {outputNames.join(", ")}</div>}
          </div>
        )}
      </div>
      {!hasFileOutputs && (
        <Handle type="source" position={Position.Right} id="dep-source" />
      )}
      {hasFileOutputs && (
        <Handle type="source" position={Position.Right} id="dep-source"
          className="hiddenHandle" />
      )}
      {exposeErrorSignal && (
        <Handle
          type="source"
          position={Position.Right}
          id="error-signal"
          title="error signal"
          style={{ top: "85%", background: "rgba(255,120,120,0.9)", borderColor: "rgba(255,120,120,0.9)" }}
        />
      )}
      {outputNames.map((name, idx) => (
        <Handle
          key={`out:${name}`}
          type="source"
          position={Position.Right}
          id={`out:${name}`}
          style={{ top: `${50 + (idx + 1) * 16}%` }}
          className="dataflowHandle dataflowHandleOutput"
          title={`output: ${name}`}
        />
      ))}
    </div>
  );
}
