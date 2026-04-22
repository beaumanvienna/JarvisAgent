import { useState, useEffect, useCallback } from "react";
import "./App.css";
import StatusBar from "./components/StatusBar";
import LastRunsBar from "./components/LastRunsBar";
import WorkflowsPanel from "./components/WorkflowsPanel";
import LogViewerPanel from "./components/LogViewerPanel";
import SettingsModal from "./components/SettingsModal";
import MasterPasswordDialog from "./components/MasterPasswordDialog";
import AdminLoginDialog from "./components/AdminLoginDialog";
import ActivationDialog from "./components/ActivationDialog";
import { useWebSocket } from "./hooks/useWebSocket";
import { usePolling } from "./hooks/usePolling";
import { shutdown, fetchKeysStatus } from "./api";
import { whoami, serverLogout } from "./auth";

type Tab = "dashboard" | "log";

export default function App() {
  const [activeTab, setActiveTab] = useState<Tab>(() => {
    const params = new URLSearchParams(window.location.search);
    return params.get("tab") === "log" ? "log" : "dashboard";
  });
  const [showSettings, setShowSettings] = useState(() => {
    // Deep-link: ?tab=mcp-keys or ?settings=1 opens the Settings modal on mount.
    const params = new URLSearchParams(window.location.search);
    return params.get("tab") === "mcp-keys" || params.get("settings") === "1";
  });
  const ws = useWebSocket();
  const { status, workflows, hasProviders, refresh } = usePolling(5000);
  const isStudio = status?.edition === "studio";
  const canRunWorkflows = status?.capabilities?.workflow_run_endpoint !== false;

  // Auth state: Engine edition requires a session cookie set by
  // POST /api/auth/login with an MCP API key.
  const isEngine = status?.edition === "engine";
  const [needsAuth, setNeedsAuth] = useState(false);
  const [authUser, setAuthUser] = useState<string | null>(null);
  const [authRole, setAuthRole] = useState<string | null>(null);
  const [showActivation, setShowActivation] = useState(false);
  const [prefillApiKey, setPrefillApiKey] = useState<string | undefined>(undefined);
  const [prefillEnrollToken, setPrefillEnrollToken] = useState<string | undefined>(undefined);

  // Key-store readiness: the first decision on every page load is "is the
  // encrypted key store unlocked?". While we don't know (`loading`) we suppress
  // all auth dialogs; when the backend replies with `ok` the sign-in flow is
  // allowed; when the backend replies with a sealed status the master-password
  // dialog takes over regardless of any auth state.
  const [keysStatus, setKeysStatus] = useState<
    "loading" | "ok" | "no_password" | "wrong_password" | "no_keys_file"
  >("loading");
  const keysSealed =
    keysStatus === "no_password" ||
    keysStatus === "wrong_password" ||
    keysStatus === "no_keys_file";

  // Probe current auth state via whoami (Engine only; Studio is open).
  useEffect(() => {
    let cancelled = false;
    whoami().then((result) => {
      if (cancelled) return;
      if (result && result.ok && result.user) {
        setAuthUser(result.user);
        setAuthRole(result.role ?? null);
        setNeedsAuth(false);
      } else if (isEngine) {
        setNeedsAuth(true);
      }
    });
    return () => {
      cancelled = true;
    };
  }, [isEngine]);

  // Listen for auth-required events from authFetch (401/403 responses).
  // We still raise `needsAuth` so the sign-in dialog surfaces once keys are
  // confirmed unlocked — but the dialog itself is gated on `keysStatus === "ok"`
  // below, so a 401 from whoami while the store is sealed can't stomp on the
  // master-password prompt.
  useEffect(() => {
    const handler = () => setNeedsAuth(true);
    window.addEventListener("j9t-auth-required", handler);
    return () => window.removeEventListener("j9t-auth-required", handler);
  }, []);

  const handleAuthenticated = useCallback(async () => {
    setNeedsAuth(false);
    const w = await whoami();
    if (w?.ok && w.user) {
      setAuthUser(w.user);
      setAuthRole(w.role ?? null);
    }
    refresh();
  }, [refresh]);

  const handleLogout = useCallback(async () => {
    await serverLogout();
    setAuthUser(null);
    setAuthRole(null);
    setNeedsAuth(true);
  }, []);

  useEffect(() => {
    fetchKeysStatus()
      .then((data) => {
        // `no_keys_file` falls through to the same dialog in "set master
        // password" mode so a fresh install can bootstrap the encrypted store
        // and receive the first-run enrollment banner.
        if (
          data.status === "ok" ||
          data.status === "no_password" ||
          data.status === "wrong_password" ||
          data.status === "no_keys_file"
        ) {
          setKeysStatus(data.status);
        }
      })
      .catch(() => {
        // Server not ready yet or network hiccup — leave state at "loading"
        // so neither dialog shows. The usePolling(5000) tick will retry via a
        // subsequent status refresh.
      });
  }, []);

  const handleKeysUnlocked = () => {
    setKeysStatus("ok");
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
      {/* Master-password dialog takes priority over sign-in: until the key store
          is unlocked no MCP key can be validated, so asking for one first would
          just yield a misleading "invalid key" error. Sign-in is suppressed
          entirely while the key store is sealed OR while the status is still
          loading — this closes a mount-time race where a 401 on whoami would
          flip `needsAuth=true` before fetchKeysStatus resolved. */}
      {keysSealed && (
        <MasterPasswordDialog
          reason={keysStatus as "no_password" | "wrong_password" | "no_keys_file"}
          onUnlocked={handleKeysUnlocked}
        />
      )}
      {keysStatus === "ok" && needsAuth && (
        <AdminLoginDialog
          onAuthenticated={handleAuthenticated}
          onOpenActivation={(prefillToken) => {
            setPrefillEnrollToken(prefillToken);
            setShowActivation(true);
          }}
          prefillApiKey={prefillApiKey}
        />
      )}
      {showActivation && (
        <ActivationDialog
          onClose={() => {
            setShowActivation(false);
            setPrefillEnrollToken(undefined);
          }}
          onActivated={(apiKey) => {
            setPrefillApiKey(apiKey);
            setPrefillEnrollToken(undefined);
            setShowActivation(false);
            setNeedsAuth(true);
          }}
          prefillToken={prefillEnrollToken}
        />
      )}
      <StatusBar
        connected={ws.connected}
        runs={ws.runs}
        aiCallsInflight={status?.ai_calls_inflight ?? 0}
        pythonRunning={ws.pythonRunning}
        mcpConnected={status?.mcp_connected ?? false}
        connectionHealth={status?.connection_health}
        totalCompleted={ws.totalCompleted}
        totalFailed={ws.totalFailed}
        onQuit={handleQuit}
        activeTab={activeTab}
        onTabChange={setActiveTab}
        isStudio={isStudio}
        onLogout={isEngine && authUser ? handleLogout : undefined}
        authUser={authUser}
        authRole={authRole}
        onOpenSettings={() => setShowSettings(true)}
      />
      <LastRunsBar lastRuns={ws.lastRuns} />
      {activeTab === "dashboard" && (
        <main className="main-content">
          <WorkflowsPanel
            workflows={workflows}
            hasProviders={hasProviders}
            runs={ws.runs}
            lastRuns={ws.lastRuns}
            onRefresh={refresh}
            canRunWorkflows={canRunWorkflows}
          />
        </main>
      )}
      {activeTab === "log" && (
        <LogViewerPanel registerLogCallback={ws.registerLogCallback} />
      )}
      {showSettings && (
        <SettingsModal
          onClose={() => setShowSettings(false)}
          role={authRole}
        />
      )}
    </>
  );
}
