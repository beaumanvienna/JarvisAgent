import type { RunSnapshot, SessionStatus, ConnectionHealthEntry } from "../types";

type Tab = "dashboard" | "log";

interface Props {
  connected: boolean;
  runs: RunSnapshot[];
  sessions: Map<string, SessionStatus>;
  pythonRunning: boolean;
  mcpConnected: boolean;
  connectionHealth?: ConnectionHealthEntry[];
  totalCompleted: number;
  totalFailed: number;
  onQuit: () => void;
  activeTab: Tab;
  onTabChange: (tab: Tab) => void;
  isStudio: boolean;
  onLogout?: () => void;
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
  mcpConnected,
  connectionHealth,
  totalCompleted,
  totalFailed,
  onQuit,
  activeTab,
  onTabChange,
  isStudio,
  onLogout,
}: Props) {
  const anyInflight = Array.from(sessions.values()).some(
    (s) => s.inflight > 0
  );
  const anyRunning = runs.some(
    (r) => r.state === "running" || r.state === "queued" || r.state === "pending"
  );

  const connectionColor = connected ? "#22c55e" : "#ef4444";
  const inflightColor = anyInflight ? "#eab308" : "#334155";
  const workflowColor = anyRunning ? "#3b82f6" : "#334155";
  const mcpColor = mcpConnected ? "#a855f7" : "#334155";
  const hasOpenCircuit = connectionHealth?.some((c) => c.circuit_state === "open") ?? false;
  const hasHalfOpen = connectionHealth?.some((c) => c.circuit_state === "half_open") ?? false;
  const cloudHealthColor = hasOpenCircuit ? "#ef4444" : hasHalfOpen ? "#eab308" : connectionHealth?.length ? "#22c55e" : "#334155";
  const cloudHealthLabel = hasOpenCircuit ? "Cloud: circuit open" : hasHalfOpen ? "Cloud: recovering" : connectionHealth?.length ? "Cloud: healthy" : "Cloud: no connections";

  return (
    <header className="status-bar">
      <div className="status-bar-left">
        <span className="title">JarvisAgent Dashboard</span>
        <span
          className="edition-badge"
          style={{
            fontSize: "0.65rem",
            padding: "1px 6px",
            borderRadius: 3,
            background: isStudio ? "#3b82f6" : "#64748b",
            color: "#fff",
            marginLeft: 8,
            verticalAlign: "middle",
            letterSpacing: "0.04em",
          }}
        >
          {isStudio ? "Studio" : "Engine"}
        </span>
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
          <Led
            color={mcpColor}
            label={mcpConnected ? "MCP connected" : "MCP offline"}
          />
          <Led color={cloudHealthColor} label={cloudHealthLabel} />
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
        <div className="tab-bar">
          <button
            className={`tab-btn ${activeTab === "dashboard" ? "tab-btn-active" : ""}`}
            onClick={() => onTabChange("dashboard")}
          >
            Dashboard
          </button>
          <button
            className={`tab-btn ${activeTab === "log" ? "tab-btn-active" : ""}`}
            onClick={() => onTabChange("log")}
          >
            Log
          </button>
        </div>
        {isStudio && (
          <a href="/editor" className="btn btn-editor">
            Workflow Editor
          </a>
        )}
        <button className="btn btn-quit" onClick={onQuit}>
          Quit
        </button>
        {onLogout && (
          <button className="btn btn-quit" onClick={onLogout} title="Clear admin token">
            Logout
          </button>
        )}
      </div>
    </header>
  );
}
