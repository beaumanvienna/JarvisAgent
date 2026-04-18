import type { WorkflowEntry, RunSnapshot, LastRunInfo } from "../types";
import { runWorkflow } from "../api";

interface Props {
  workflows: WorkflowEntry[];
  hasProviders: boolean;
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

export default function WorkflowsPanel({ workflows, hasProviders, runs, lastRuns, onRefresh, canRunWorkflows }: Props) {
  const topLevelWorkflows = workflows.filter((wf) => !wf.is_sub_workflow);

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
      <h2>Workflows</h2>
      {!hasProviders && topLevelWorkflows.some((wf) => wf.has_ai_call) && (
        <div className="no-keys-banner">
          No AI providers configured — workflows with ai_call tasks have been
          skipped. Unlock the master password, or add providers in the Settings
          UI in the workflow editor, then reload workflows.
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
