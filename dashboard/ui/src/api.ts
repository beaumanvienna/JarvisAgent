import type { StatusResponse, WorkflowsListResponse, LastRunsResponse, LogResponse, AnalyzeLastRunResponse } from "./types";

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

export async function fetchLog(opts: { tail?: number; offset?: number }): Promise<LogResponse> {
  const params = new URLSearchParams();
  if (opts.offset !== undefined) {
    params.set("offset", String(opts.offset));
  } else if (opts.tail !== undefined) {
    params.set("tail", String(opts.tail));
  }
  const res = await fetch(`${BASE}/api/log?${params.toString()}`);
  return res.json();
}

export async function fetchLogAnalyzeLastRun(index?: number): Promise<AnalyzeLastRunResponse> {
  const params = index !== undefined && index > 0 ? `?index=${index}` : "";
  const res = await fetch(`${BASE}/api/log/analyze-last-run${params}`);
  return res.json();
}
