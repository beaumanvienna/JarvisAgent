import type { RunSnapshot, ConnectionHealthEntry } from "../types";

type Tab = "dashboard" | "log";

interface Props {
  connected: boolean;
  runs: RunSnapshot[];
  aiCallsInflight: number;
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
  authUser?: string | null;
  authRole?: string | null;
  onOpenSettings?: () => void;
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
  aiCallsInflight,
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
  authUser,
  authRole,
  onOpenSettings,
}: Props) {
  const anyInflight = aiCallsInflight > 0;
  const anyRunning = runs.some(
    (r) => r.state === "running" || r.state === "queued" || r.state === "pending"
  );

  const connectionColor = connected ? "#22c55e" : "#ef4444";
  const inflightColor = anyInflight ? "#eab308" : "#334155";
  const workflowColor = anyRunning ? "#3b82f6" : "#334155";
  const mcpColor = mcpConnected ? "#a855f7" : "#334155";
  const hasOpenCircuit = connectionHealth?.some((c) => c.circuit_state === "open") ?? false;
  const hasHalfOpen = connectionHealth?.some((c) => c.circuit_state === "half_open") ?? false;
  // Only count connections that have actually been proved healthy (Test click
  // or JCWF success).  Merely configured → grey/unknown, not green.
  const confirmedCount = connectionHealth?.filter((c) => c.confirmed_healthy).length ?? 0;
  const cloudHealthColor = hasOpenCircuit
    ? "#ef4444"
    : hasHalfOpen
    ? "#eab308"
    : confirmedCount > 0
    ? "#22c55e"
    : "#334155";
  const cloudHealthLabel = hasOpenCircuit
    ? "Cloud: circuit open"
    : hasHalfOpen
    ? "Cloud: recovering"
    : confirmedCount > 0
    ? `Cloud: ${confirmedCount} healthy`
    : "Cloud: no connections";

  return (
    <header className="status-bar">
      <div className="status-bar-left">
        <span className="title">
          JarvisAgent Dashboard
          {authUser && (
            <span
              style={{
                marginLeft: 10,
                fontSize: 12,
                fontWeight: 400,
                color: "rgba(255,255,255,0.6)",
              }}
              title="Signed-in identity"
            >
              {authUser}
              {authRole && (
                <span
                  style={{
                    marginLeft: 6,
                    padding: "1px 5px",
                    borderRadius: 2,
                    background: authRole === "admin" ? "#7c3aed" : "#334155",
                    color: "#fff",
                    fontSize: 10,
                  }}
                >
                  {authRole}
                </span>
              )}
            </span>
          )}
        </span>
        <div className="led-row">
          <Led
            color={connectionColor}
            label={connected ? "Connected" : "Disconnected"}
          />
          <Led
            color={inflightColor}
            label={anyInflight ? `AI queries in flight (${aiCallsInflight})` : "No queries"}
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
          {activeTab !== "dashboard" && (
            <button
              className="tab-btn"
              onClick={() => onTabChange("dashboard")}
            >
              Dashboard
            </button>
          )}
          {activeTab !== "log" && (
            <button
              className="tab-btn"
              onClick={() => onTabChange("log")}
            >
              Log
            </button>
          )}
        </div>
        {onOpenSettings && (
          <button
            className="btn"
            type="button"
            onClick={onOpenSettings}
            title="Settings"
            style={{ fontSize: 18, padding: "4px 10px" }}
          >
            ⚙
          </button>
        )}
        {isStudio && (
          <a href="/editor" className="btn btn-editor">
            Workflow Editor
          </a>
        )}
        <button className="btn btn-quit" onClick={onQuit}>
          Quit
        </button>
        {onLogout && (
          <button className="btn btn-quit" onClick={onLogout} title="Sign out">
            Logout
          </button>
        )}
      </div>
    </header>
  );
}
