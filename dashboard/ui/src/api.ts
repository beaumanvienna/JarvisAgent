import type { StatusResponse, WorkflowsListResponse, LastRunsResponse, LogResponse, AnalyzeLastRunResponse, KeysStatusResponse, KeysUnlockResponse } from "./types";
import { authFetch } from "./auth";

const BASE = window.location.origin;

// /api/status is public — no auth needed.
export async function fetchStatus(): Promise<StatusResponse> {
  const res = await fetch(`${BASE}/api/status`);
  return res.json();
}

export async function fetchWorkflows(): Promise<WorkflowsListResponse> {
  const res = await authFetch(`${BASE}/api/workflows`);
  return res.json();
}

export async function runWorkflow(workflowId: string): Promise<void> {
  await authFetch(`${BASE}/api/workflows/${encodeURIComponent(workflowId)}/run`, {
    method: "POST",
  });
}

export async function reloadWorkflows(): Promise<void> {
  await authFetch(`${BASE}/api/workflows/reload`, { method: "POST" });
}

export async function fetchLastRuns(): Promise<LastRunsResponse> {
  const res = await authFetch(`${BASE}/api/workflow-runs/last`);
  return res.json();
}

export async function shutdown(): Promise<void> {
  await authFetch(`${BASE}/api/shutdown`, { method: "POST" });
}

export async function fetchLog(opts: { tail?: number; offset?: number }): Promise<LogResponse> {
  const params = new URLSearchParams();
  if (opts.offset !== undefined) {
    params.set("offset", String(opts.offset));
  } else if (opts.tail !== undefined) {
    params.set("tail", String(opts.tail));
  }
  const res = await authFetch(`${BASE}/api/log?${params.toString()}`);
  return res.json();
}

export async function fetchKeysStatus(): Promise<KeysStatusResponse> {
  const res = await authFetch(`${BASE}/api/settings/keys/status`);
  return res.json();
}

export async function unlockKeys(masterPassword: string): Promise<KeysUnlockResponse> {
  const res = await authFetch(`${BASE}/api/settings/keys/unlock`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ master_password: masterPassword }),
  });
  return res.json();
}

export async function fetchLogAnalyzeLastRun(index?: number): Promise<AnalyzeLastRunResponse> {
  const params = index !== undefined && index > 0 ? `?index=${index}` : "";
  const res = await authFetch(`${BASE}/api/log/analyze-last-run${params}`);
  return res.json();
}
