import React from "react";
import type { StatusState } from "../hooks/useStatusWebSocket";

function Led({ color, label }: { color: string; label: string }): JSX.Element
{
  return (
    <span className="statusLedGroup">
      <span
        className="statusLed"
        style={{
          background: color,
          boxShadow: color === "#334155" ? "none" : `0 0 6px ${color}, 0 0 12px ${color}`,
        }}
      />
      <span className="statusLedLabel">{label}</span>
    </span>
  );
}

type Props = { status: StatusState };

export default function StatusLeds({ status }: Props): JSX.Element
{
  const connectionColor = status.connected ? "#22c55e" : "#ef4444";
  const inflightColor = status.anyInflight ? "#eab308" : "#334155";
  const workflowColor = status.anyRunning ? "#3b82f6" : "#334155";

  return (
    <div className="statusLedRow">
      <Led color={connectionColor} label={status.connected ? "Connected" : "Disconnected"} />
      <Led color={inflightColor} label={status.anyInflight ? "Queries in flight" : "No queries"} />
      <Led color={workflowColor} label={status.anyRunning ? "Workflow running" : "No active runs"} />
      {!status.pythonRunning && (
        <span style={{ color: "#ef4444", fontWeight: 700, fontSize: "0.75rem" }}>Python Offline</span>
      )}
      <span className="statusRunCounters">
        <span style={{ color: "#22c55e" }}>{status.totalCompleted} succeeded</span>
        {status.totalFailed > 0 && (
          <span style={{ color: "#ef4444" }}>{status.totalFailed} failed</span>
        )}
      </span>
    </div>
  );
}
