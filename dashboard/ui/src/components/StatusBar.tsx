import type { RunSnapshot, SessionStatus } from "../types";

interface Props {
  connected: boolean;
  runs: RunSnapshot[];
  sessions: Map<string, SessionStatus>;
  pythonRunning: boolean;
  totalCompleted: number;
  totalFailed: number;
  onQuit: () => void;
}

function Led({ color, label }: { color: string; label: string }) {
  return (
    <span className="led-group">
      <span
        className="led"
        style={{
          background: color,
          boxShadow: color === "#334155" ? "none" : `0 0 6px ${color}, 0 0 12px ${color}`,
        }}
      />
      <span className="led-label">{label}</span>
    </span>
  );
}

export default function StatusBar({
  connected,
  runs,
  sessions,
  pythonRunning,
  totalCompleted,
  totalFailed,
  onQuit,
}: Props) {
  const anyInflight = Array.from(sessions.values()).some(
    (s) => s.inflight > 0
  );
  const anyRunning = runs.some((r) => r.state === "running");

  const connectionColor = connected ? "#22c55e" : "#ef4444";
  const inflightColor = anyInflight ? "#eab308" : "#334155";
  const workflowColor = anyRunning ? "#3b82f6" : "#334155";

  return (
    <header className="status-bar">
      <div className="status-bar-left">
        <span className="title">JarvisAgent Dashboard</span>
        <div className="led-row">
          <Led
            color={connectionColor}
            label={connected ? "Connected" : "Disconnected"}
          />
          <Led
            color={inflightColor}
            label={anyInflight ? "Queries in flight" : "No queries"}
          />
          <Led
            color={workflowColor}
            label={anyRunning ? "Workflow running" : "No active runs"}
          />
          {!pythonRunning && (
            <span className="python-warning">Python Offline</span>
          )}
          <span className="run-counters">
            <span className="counter-ok">{totalCompleted} succeeded</span>
            {totalFailed > 0 && (
              <span className="counter-fail">{totalFailed} failed</span>
            )}
          </span>
        </div>
      </div>
      <div className="status-bar-right">
        <a href="/editor" className="btn btn-editor">
          Workflow Editor
        </a>
        <button className="btn btn-quit" onClick={onQuit}>
          Quit
        </button>
      </div>
    </header>
  );
}
