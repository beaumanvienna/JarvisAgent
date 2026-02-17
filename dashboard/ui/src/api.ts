import type { StatusResponse, WorkflowsListResponse, LastRunsResponse } from "./types";

const BASE = window.location.origin;

export async function fetchStatus(): Promise<StatusResponse> {
  const res = await fetch(`${BASE}/api/status`);
  return res.json();
}

export async function fetchWorkflows(): Promise<WorkflowsListResponse> {
  const res = await fetch(`${BASE}/api/workflows`);
  return res.json();
}

export async function runWorkflow(workflowId: string): Promise<void> {
  await fetch(`${BASE}/api/workflows/${encodeURIComponent(workflowId)}/run`, {
    method: "POST",
  });
}

export async function reloadWorkflows(): Promise<void> {
  await fetch(`${BASE}/api/workflows/reload`, { method: "POST" });
}

export async function fetchLastRuns(): Promise<LastRunsResponse> {
  const res = await fetch(`${BASE}/api/workflow-runs/last`);
  return res.json();
}

export async function shutdown(): Promise<void> {
  await fetch(`${BASE}/api/shutdown`, { method: "POST" });
}
