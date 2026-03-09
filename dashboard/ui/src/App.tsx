import { useState, useEffect } from "react";
import "./App.css";
import StatusBar from "./components/StatusBar";
import WorkflowsPanel from "./components/WorkflowsPanel";
import SessionManagersPanel from "./components/SessionManagersPanel";
import LogViewerPanel from "./components/LogViewerPanel";
import MasterPasswordDialog from "./components/MasterPasswordDialog";
import { useWebSocket } from "./hooks/useWebSocket";
import { usePolling } from "./hooks/usePolling";
import { shutdown, fetchKeysStatus } from "./api";

type Tab = "dashboard" | "log";

export default function App() {
  const [activeTab, setActiveTab] = useState<Tab>(() => {
    const params = new URLSearchParams(window.location.search);
    return params.get("tab") === "log" ? "log" : "dashboard";
  });
  const ws = useWebSocket();
  const { workflows, hasProviders, refresh } = usePolling(5000);

  const [keysPromptReason, setKeysPromptReason] = useState<
    "no_password" | "wrong_password" | null
  >(null);

  useEffect(() => {
    fetchKeysStatus()
      .then((data) => {
        if (data.status === "no_password" || data.status === "wrong_password") {
          setKeysPromptReason(data.status);
        }
      })
      .catch(() => {
        // server not ready yet — ignore
      });
  }, []);

  const handleKeysUnlocked = () => {
    setKeysPromptReason(null);
    refresh();
  };

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
      {keysPromptReason && (
        <MasterPasswordDialog
          reason={keysPromptReason}
          onUnlocked={handleKeysUnlocked}
        />
      )}
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
            hasProviders={hasProviders}
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
