import "./App.css";
import StatusBar from "./components/StatusBar";
import WorkflowsPanel from "./components/WorkflowsPanel";
import SessionManagersPanel from "./components/SessionManagersPanel";
import { useWebSocket } from "./hooks/useWebSocket";
import { usePolling } from "./hooks/usePolling";
import { shutdown } from "./api";

export default function App() {
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
      />
      <main className="main-content">
        <WorkflowsPanel
          workflows={workflows}
          runs={ws.runs}
          lastRuns={ws.lastRuns}
          onRefresh={refresh}
        />
        <SessionManagersPanel sessions={ws.sessions} />
      </main>
    </>
  );
}
