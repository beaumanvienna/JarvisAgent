import { useState } from "react";
import type {
  WorkflowEntry,
  RunSnapshot,
  LastRunInfo,
  ProviderAlertEntry,
  ProviderErrorCategory,
} from "../types";
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
  // Sitting-7 Workstream C: active per-interface provider alerts.  Map keyed by
  // `${interfaceName}|${category}`.  Banner UI + hazard glyph branch off this.
  providerAlerts: Map<string, ProviderAlertEntry>;
  onDismissProviderAlert: (key: string) => void;
}

// Copy templates per §5.3 of doc/misc/ai-provider-error-visibility-dev-plan.md.
// Title is provider-agnostic — phrasing uses the user-configured interface label,
// never a provider brand name (per the dev plan §1 "no hardcoded provider names").
const CATEGORY_COPY: Record<ProviderErrorCategory, { title: string; verb: string } | null> = {
  Unknown: null,                              // not banner-worthy on its own
  ThrottleRateLimit: null,                    // AIMD's job — surfaced via AI Health LED
  InvalidRequest: null,                       // caller bug, not provider state
  BillingExhausted:
    { title: "Account billing limit reached",  verb: "is rejecting requests for billing reasons" },
  AuthFailure:
    { title: "Interface credentials rejected", verb: "is rejecting requests — credentials failed validation" },
  ServiceOverload:
    { title: "Interface overloaded — retrying", verb: "is reporting transient overload" },
  ModelNotFound:
    { title: "Requested model not available", verb: "rejected the request — model is unavailable or deprecated" },
};

function alertKey(interfaceName: string, category: ProviderErrorCategory): string {
  return `${interfaceName}|${category}`;
}

// Returns the most-severe ongoing category for an interface, or null if none.
// Severity order matches §5.3: BillingExhausted is the loudest (red banner +
// stalls workflows), then AuthFailure (also a hard stop), then ModelNotFound,
// then ServiceOverload (informational; auto-clears).
const SEVERITY_ORDER: ProviderErrorCategory[] = [
  "BillingExhausted",
  "AuthFailure",
  "ModelNotFound",
  "ServiceOverload",
];

function worstCategoryFor(
  interfaceName: string,
  alerts: Map<string, ProviderAlertEntry>,
): ProviderErrorCategory | null {
  for (const cat of SEVERITY_ORDER) {
    if (alerts.has(alertKey(interfaceName, cat))) return cat;
  }
  return null;
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
      return "state-succeeded";
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
  providerAlerts,
  onDismissProviderAlert,
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

  // Separate active (running/cancelled/etc.) from pending runs.  Serialize policy
  // can produce N pending runs alongside one active run for the same workflowId;
  // last-write-wins on a single map would hide the active state behind the last
  // pending entry.  Show the active state primary, with a "+N queued" badge.
  const activeByWorkflow = new Map<string, RunSnapshot>();
  const pendingCountByWorkflow = new Map<string, number>();
  for (const run of runs) {
    if (run.state === "pending") {
      pendingCountByWorkflow.set(run.workflowId, (pendingCountByWorkflow.get(run.workflowId) ?? 0) + 1);
    } else {
      activeByWorkflow.set(run.workflowId, run);
    }
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
      {/* Sitting-7 Workstream C: one banner per (interface, actionable-category)
          pair.  Dedup happens at insert time in useWebSocket; this is a pure render. */}
      {Array.from(providerAlerts.entries())
        .map(([key, alert]) => {
          const copy = CATEGORY_COPY[alert.category];
          if (copy === null) return null;
          return (
            <div className="provider-alert-banner" key={key}>
              <div className="provider-alert-body">
                <strong>{copy.title}</strong>
                <span>
                  {" "}— Interface <code>{alert.interfaceName}</code> {copy.verb}
                  {alert.errorType ? (
                    <>
                      {" "}(<code>{alert.errorType}</code>
                      {alert.errorCode && alert.errorCode !== alert.errorType
                        ? ` / ${alert.errorCode}`
                        : ""}
                      )
                    </>
                  ) : null}
                  . Affected calls: {alert.count}.
                  {alert.retryAfterSeconds !== undefined
                    ? ` Provider asks for ${alert.retryAfterSeconds}s before retry.`
                    : ""}
                </span>
              </div>
              <button
                className="provider-alert-dismiss"
                type="button"
                aria-label="Dismiss"
                title="Dismiss for this session"
                onClick={() => onDismissProviderAlert(key)}
              >
                ×
              </button>
            </div>
          );
        })
        .filter(Boolean)}
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
              const activeRun = activeByWorkflow.get(wf.id);
              const pendingCount = pendingCountByWorkflow.get(wf.id) ?? 0;
              const lastRun = lastRunByWorkflow.get(wf.id);
              // If any pending runs are queued, force "running" — the workflow as
              // a whole is still in flight, even when the current activeRun is
              // briefly in the "succeeded" min-visibility hold between FIFO drains.
              // Without this, the cell flickers running → succeeded → running every
              // run boundary, which reads as glitchy when 3+ runs are queued.
              const displayState =
                pendingCount > 0
                  ? "running"
                  : activeRun?.state ?? lastRun?.state ?? null;
              const isRunning =
                displayState === "running" ||
                displayState === "pending" ||
                displayState === "queued" ||
                pendingCount > 0;
              const missingKeys = !hasProviders && !!wf.has_ai_call;
              // Sitting-7 Workstream C: scan this workflow's ai_call interfaces against the
              // active alert map.  worst-category wins (Billing > Auth > ModelNotFound >
              // ServiceOverload per SEVERITY_ORDER) — one glyph, one tooltip per row.
              let degradedInterface: string | null = null;
              let degradedCategory: ProviderErrorCategory | null = null;
              if (!missingKeys && wf.interface_names && wf.interface_names.length > 0) {
                for (const iface of wf.interface_names) {
                  if (!iface) continue;          // empty string = "system default" — skip
                  const cat = worstCategoryFor(iface, providerAlerts);
                  if (cat !== null) {
                    degradedInterface = iface;
                    degradedCategory = cat;
                    break;
                  }
                }
              }
              return (
                <tr key={wf.id} className={missingKeys ? "row-no-keys" : ""}>
                  <td className="mono">
                    {wf.id}
                    {missingKeys && (
                      <span className="hazard-icon" title="AI provider keys not configured — cannot run this workflow">
                        &#9888;
                      </span>
                    )}
                    {degradedInterface && degradedCategory && (
                      <span
                        className="hazard-icon hazard-icon-red"
                        title={`AI interface '${degradedInterface}': ${degradedCategory}`}
                      >
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
                    {pendingCount > 0 && (
                      <span
                        className="queued-badge"
                        title={`${pendingCount} run${pendingCount === 1 ? "" : "s"} queued (serialize policy)`}
                      >
                        +{pendingCount} queued
                      </span>
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
