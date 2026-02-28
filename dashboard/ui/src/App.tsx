import { useState } from "react";
import "./App.css";
import StatusBar from "./components/StatusBar";
import WorkflowsPanel from "./components/WorkflowsPanel";
import SessionManagersPanel from "./components/SessionManagersPanel";
import LogViewerPanel from "./components/LogViewerPanel";
import { useWebSocket } from "./hooks/useWebSocket";
import { usePolling } from "./hooks/usePolling";
import { shutdown } from "./api";

type Tab = "dashboard" | "log";

export default function App() {
  const [activeTab, setActiveTab] = useState<Tab>(() => {
    const params = new URLSearchParams(window.location.search);
    return params.get("tab") === "log" ? "log" : "dashboard";
  });
  const ws = useWebSocket();
  const { workflows, refresh } = usePolling(5000);

  const handleQuit = async () => {
    if (!window.confirm("Shut down JarvisAgent?")) return;
    try {
      await shutdown();
    } catch {
      // server gone
    }
  };

  return (
    <>
      <StatusBar
        connected={ws.connected}
        runs={ws.runs}
        sessions={ws.sessions}
        pythonRunning={ws.pythonRunning}
        totalCompleted={ws.totalCompleted}
        totalFailed={ws.totalFailed}
        onQuit={handleQuit}
        activeTab={activeTab}
        onTabChange={setActiveTab}
      />
      {activeTab === "dashboard" ? (
        <main className="main-content">
          <WorkflowsPanel
            workflows={workflows}
            runs={ws.runs}
            lastRuns={ws.lastRuns}
            onRefresh={refresh}
          />
          <SessionManagersPanel sessions={ws.sessions} />
        </main>
      ) : (
        <LogViewerPanel />
      )}
    </>
  );
}
