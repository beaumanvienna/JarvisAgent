import { useState } from "react";
import type { WorkflowEntry, RunSnapshot, LastRunInfo } from "../types";
import { reloadWorkflows, runWorkflow } from "../api";

interface Props {
  workflows: WorkflowEntry[];
  hasProviders: boolean;
  keysSealed: boolean;
  onRequestUnlock: () => void;
  runs: RunSnapshot[];
  lastRuns: LastRunInfo[];
  onRefresh: () => void;
  canRunWorkflows: boolean;
}

function progressText(run: RunSnapshot): string {
  const total = run.tasks.length;
  if (total === 0) return run.state;
  const done = run.tasks.filter(
    (t) => t.state === "succeeded" || t.state === "skipped"
  ).length;
  const failed = run.tasks.filter((t) => t.state === "failed").length;
  let text = `${done}/${total}`;
  if (failed > 0) text += ` (${failed} failed)`;
  return text;
}

function stateClass(state: string): string {
  switch (state) {
    case "running":
      return "state-running";
    case "succeeded":
      return "state-completed";
    case "failed":
      return "state-failed";
    case "queued":
    case "pending":
      return "state-queued";
    case "cancelled":
      return "state-cancelled";
    default:
      return "";
  }
}

export default function WorkflowsPanel({
  workflows,
  hasProviders,
  keysSealed,
  onRequestUnlock,
  runs,
  lastRuns,
  onRefresh,
  canRunWorkflows,
}: Props) {
  const [reloading, setReloading] = useState(false);
  const topLevelWorkflows = workflows.filter((wf) => !wf.is_sub_workflow);

  const handleReload = async () => {
    setReloading(true);
    try {
      await reloadWorkflows();
      onRefresh();
    } catch {
      // silent — toast/status would go here if we had one
    } finally {
      setReloading(false);
    }
  };

  const runsByWorkflow = new Map<string, RunSnapshot>();
  for (const run of runs) {
    runsByWorkflow.set(run.workflowId, run);
  }

  const lastRunByWorkflow = new Map<string, LastRunInfo>();
  for (const lr of lastRuns) {
    lastRunByWorkflow.set(lr.workflowId, lr);
  }

  const handleRun = async (id: string) => {
    try {
      await runWorkflow(id);
      onRefresh();
    } catch {
      // silent
    }
  };

  return (
    <section className="panel">
      <div style={{ display: "flex", alignItems: "center", gap: 12, marginBottom: 8 }}>
        <h2 style={{ margin: 0 }}>Workflows</h2>
        <button
          className="btn"
          type="button"
          onClick={handleReload}
          disabled={reloading}
          title="Re-scan the workflows folder for new or modified .jcwf files"
        >
          {reloading ? "Refreshing…" : "Refresh"}
        </button>
      </div>
      {!hasProviders && topLevelWorkflows.some((wf) => wf.has_ai_call) && (
        <div className="no-keys-banner">
          {keysSealed ? (
            <>
              <span>
                Master password not entered — AI provider keys are encrypted in
                <code> keys.json.enc</code> and unreachable until you unlock.
                Workflows with <code>ai_call</code> tasks have been skipped.
              </span>
              <button
                className="btn btn-editor"
                type="button"
                onClick={onRequestUnlock}
                style={{ marginLeft: 12 }}
              >
                Unlock master password
              </button>
            </>
          ) : (
            <>
              No AI provider keys configured — workflows with <code>ai_call</code>
              {" "}tasks have been skipped. Add a provider in the AI Manager in the
              workflow editor, then reload workflows.
            </>
          )}
        </div>
      )}
      {topLevelWorkflows.length === 0 ? (
        <p className="muted">No workflows loaded.</p>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th>ID</th>
              <th>Label</th>
              <th>State</th>
              <th>Progress</th>
              <th>Actions</th>
            </tr>
          </thead>
          <tbody>
            {topLevelWorkflows.map((wf) => {
              const activeRun = runsByWorkflow.get(wf.id);
              const lastRun = lastRunByWorkflow.get(wf.id);
              const displayState = activeRun?.state ?? lastRun?.state ?? null;
              const isRunning = displayState === "running" || displayState === "pending" || displayState === "queued";
              const missingKeys = !hasProviders && !!wf.has_ai_call;
              return (
                <tr key={wf.id} className={missingKeys ? "row-no-keys" : ""}>
                  <td className="mono">
                    {wf.id}
                    {missingKeys && (
                      <span className="hazard-icon" title="AI provider keys not configured — cannot run this workflow">
                        &#9888;
                      </span>
                    )}
                  </td>
                  <td>{wf.label || "\u2014"}</td>
                  <td>
                    {displayState ? (
                      <span className={stateClass(displayState)}>
                        {displayState}
                      </span>
                    ) : (
                      <span className="muted">idle</span>
                    )}
                  </td>
                  <td className="mono">
                    {activeRun
                      ? progressText(activeRun)
                      : lastRun
                        ? `${lastRun.taskCount} tasks`
                        : "\u2014"}
                  </td>
                  <td>
                    {canRunWorkflows && wf.manual_start && (
                      <button
                        className="btn btn-small btn-run"
                        onClick={() => handleRun(wf.id)}
                        disabled={isRunning || missingKeys}
                        title={missingKeys ? "AI provider keys not configured" : undefined}
                      >
                        Run
                      </button>
                    )}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}
    </section>
  );
}
