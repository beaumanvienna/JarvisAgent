import type { StatusResponse, WorkflowsListResponse, LastRunsResponse, LogResponse, AnalyzeLastRunResponse, ProvidersHealthResponse } from "./types";
import type { KeysStatusResponse, KeysUnlockResponse } from "@shared/api/keys";
import { authFetch } from "@shared/api/auth";

// Sitting-8 Workstream D: per-interface health snapshot driving the AI Health
// LED.  Polled from usePolling; gracefully returns empty interfaces on 401
// (engine-mode auth gap before sign-in).
export async function fetchProvidersHealth(): Promise<ProvidersHealthResponse> {
  const res = await authFetch(`${BASE}/api/providers/health`);
  return res.json();
}

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

export async function fetchSecurityLog(opts: { tail?: number; offset?: number }): Promise<LogResponse> {
  const params = new URLSearchParams();
  if (opts.offset !== undefined) {
    params.set("offset", String(opts.offset));
  } else if (opts.tail !== undefined) {
    params.set("tail", String(opts.tail));
  }
  const res = await authFetch(`${BASE}/api/log/security?${params.toString()}`);
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

// ---- MCP API keys ---------------------------------------------------------

export interface McpKeyRecord {
  key_id: string;
  user: string;
  role: "admin" | "operator" | "viewer";
  adhoc_enabled: boolean;
  disk_quota_mb: number;
  default_cleanup_policy: string;
  created_at: string;
  expires_at: string;
  last_used_at: string;
  enabled: boolean;
  description: string;
}

export interface McpKeysListResponse {
  ok: boolean;
  keys: McpKeyRecord[];
  error?: string;
}

export interface McpEnrollmentRequest {
  user: string;
  role: "admin" | "operator" | "viewer";
  adhoc_enabled?: boolean;
  disk_quota_mb?: number;
  default_cleanup_policy?: string;
  description?: string;
  key_expiry_days?: number;
  enrollment_ttl_minutes?: number;
}

export interface McpEnrollmentResponse {
  ok: boolean;
  enrollment_token: string;
  expires_in_minutes: number;
  user: string;
  role: string;
  message: string;
}

export interface ScriptEntry {
  path: string;
  type: "shell" | "python";
  module?: string;
  short?: string;
  description?: string;
  outputs?: string;
  params?: string[];
  has_shebang?: boolean;
  has_jarvis_marker?: boolean;
  executable?: boolean;
}

export interface ScriptsListResponse {
  ok: boolean;
  count?: number;
  scripts?: ScriptEntry[];
  error?: string;
  message?: string;
}

export async function fetchScripts(opts: { type?: "shell" | "python"; refresh?: boolean } = {}): Promise<ScriptsListResponse> {
  const qs = new URLSearchParams();
  if (opts.type) qs.set("type", opts.type);
  if (opts.refresh) qs.set("refresh", "1");
  const query = qs.toString() ? `?${qs.toString()}` : "";
  const res = await authFetch(`${BASE}/api/scripts${query}`);
  return res.json();
}

export async function fetchMcpKeys(): Promise<McpKeysListResponse> {
  const res = await authFetch(`${BASE}/api/auth/mcp-keys`);
  return res.json();
}

export async function createMcpEnrollment(
  req: McpEnrollmentRequest
): Promise<McpEnrollmentResponse> {
  const res = await authFetch(`${BASE}/api/auth/mcp-keys/enroll`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(req),
  });
  return res.json();
}

export async function revokeMcpKey(keyId: string): Promise<{ ok: boolean }> {
  const res = await authFetch(
    `${BASE}/api/auth/mcp-keys/${encodeURIComponent(keyId)}`,
    { method: "DELETE" }
  );
  return res.json();
}

export async function updateMcpKey(
  keyId: string,
  fields: Partial<Pick<McpKeyRecord, "role" | "adhoc_enabled" | "disk_quota_mb" | "default_cleanup_policy" | "enabled" | "description" | "expires_at">>
): Promise<{ ok: boolean }> {
  const res = await authFetch(
    `${BASE}/api/auth/mcp-keys/${encodeURIComponent(keyId)}`,
    {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(fields),
    }
  );
  return res.json();
}
