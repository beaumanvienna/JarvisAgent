import { useState, useEffect, useCallback } from "react";
import "./App.css";
import StatusBar from "./components/StatusBar";
import WorkflowsPanel from "./components/WorkflowsPanel";
import SessionManagersPanel from "./components/SessionManagersPanel";
import LogViewerPanel from "./components/LogViewerPanel";
import MasterPasswordDialog from "./components/MasterPasswordDialog";
import AdminLoginDialog from "./components/AdminLoginDialog";
import { useWebSocket } from "./hooks/useWebSocket";
import { usePolling } from "./hooks/usePolling";
import { shutdown, fetchKeysStatus } from "./api";
import { getToken, clearToken } from "./auth";

type Tab = "dashboard" | "log";

export default function App() {
  const [activeTab, setActiveTab] = useState<Tab>(() => {
    const params = new URLSearchParams(window.location.search);
    return params.get("tab") === "log" ? "log" : "dashboard";
  });
  const ws = useWebSocket();
  const { status, workflows, hasProviders, refresh } = usePolling(5000);
  const isStudio = status?.edition === "studio";
  const canRunWorkflows = status?.capabilities?.workflow_run_endpoint !== false;

  // Auth state: Engine edition requires a token.
  const isEngine = status?.edition === "engine";
  const [needsAuth, setNeedsAuth] = useState(false);

  // Check if we need auth: Engine edition + no stored token.
  useEffect(() => {
    if (isEngine && !getToken()) {
      setNeedsAuth(true);
    }
  }, [isEngine]);

  // Listen for auth-required events from authFetch (401/403 responses).
  useEffect(() => {
    const handler = () => setNeedsAuth(true);
    window.addEventListener("j9t-auth-required", handler);
    return () => window.removeEventListener("j9t-auth-required", handler);
  }, []);

  const handleAuthenticated = useCallback(() => {
    setNeedsAuth(false);
    refresh();
  }, [refresh]);

  const handleLogout = useCallback(() => {
    clearToken();
    setNeedsAuth(true);
  }, []);

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
      {needsAuth && <AdminLoginDialog onAuthenticated={handleAuthenticated} />}
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
        mcpConnected={status?.mcp_connected ?? false}
        totalCompleted={ws.totalCompleted}
        totalFailed={ws.totalFailed}
        onQuit={handleQuit}
        activeTab={activeTab}
        onTabChange={setActiveTab}
        isStudio={isStudio}
        onLogout={isEngine && getToken() ? handleLogout : undefined}
      />
      {activeTab === "dashboard" ? (
        <main className="main-content">
          <WorkflowsPanel
            workflows={workflows}
            hasProviders={hasProviders}
            runs={ws.runs}
            lastRuns={ws.lastRuns}
            onRefresh={refresh}
            canRunWorkflows={canRunWorkflows}
          />
          <SessionManagersPanel sessions={ws.sessions} />
        </main>
      ) : (
        <LogViewerPanel registerLogCallback={ws.registerLogCallback} />
      )}
    </>
  );
}
