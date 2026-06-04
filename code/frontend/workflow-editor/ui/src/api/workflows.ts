import { authFetch } from "@shared/api/auth";

export type WorkflowListItem = {
  id: string;
  label?: string;
  path?: string;
  manual_start?: boolean;
  is_sub_workflow?: boolean;
  container_path?: string;
  container_folder?: string;
  parent_workflow_id?: string;
};

export type BrokenWorkflowItem = {
  id: string;
  path?: string;
  error: string;
};

export type WorkflowListResponse = {
  workflows: WorkflowListItem[];
  broken?: BrokenWorkflowItem[];
};

export type WorkflowValidationFinding = {
  code: string;
  message: string;
  path?: string;
  taskId?: string;
  tier?: "A" | "B" | "C" | "D";
};

export type WorkflowValidationResponse = {
  ok: boolean;
  id: string;
  errors: WorkflowValidationFinding[];
  warnings: WorkflowValidationFinding[];
  infos: WorkflowValidationFinding[];
};

export type WorkflowPersistResponse = {
  ok: boolean;
  id: string;
  savedPath?: string;
};

export type WorkflowRunResponse = {
  ok: boolean;
  id: string;
  runId: string;
  enqueued: boolean;
};

export type WorkflowDeleteResponse = {
  ok: boolean;
  id: string;
  deleted: boolean;
};

function ensureOk(response: Response): void
{
  if (!response.ok)
  {
    throw new Error(`HTTP ${response.status} ${response.statusText}`);
  }
}

function withWorkflowId(workflowId: string, workflowJson: unknown): unknown
{
  if (workflowJson && typeof workflowJson === "object" && !Array.isArray(workflowJson))
  {
    const existing = workflowJson as Record<string, unknown>;
    // Do not mutate caller input.
    return { ...existing, id: workflowId };
  }
  return workflowJson;
}

export async function reloadWorkflowRegistry(): Promise<void>
{
  const response = await authFetch("/api/workflows/reload", { method: "POST" });
  ensureOk(response);
}

export async function listWorkflows(): Promise<WorkflowListResponse>
{
  const response = await authFetch("/api/workflows");
  ensureOk(response);
  return (await response.json()) as WorkflowListResponse;
}

/**
 * GET /api/workflows/{id}
 * Backend returns the raw JCWF JSON body (canonical) as the response.
 */
export async function loadWorkflow(workflowId: string): Promise<unknown | null>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}`);
  if (response.status === 404)
  {
    return null;
  }
  ensureOk(response);
  return await response.json();
}

export async function createWorkflow(workflowJson: unknown): Promise<WorkflowPersistResponse>
{
  const response = await authFetch("/api/workflows", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(workflowJson),
  });
  ensureOk(response);
  return (await response.json()) as WorkflowPersistResponse;
}

/**
 * Convenience: create a workflow with a specific id.
 * This mirrors the UI behavior where the user picks the id.
 */
export async function createWorkflowWithId(workflowId: string, workflowJson: unknown): Promise<WorkflowPersistResponse>
{
  return await createWorkflow(withWorkflowId(workflowId, workflowJson));
}

export async function saveWorkflow(workflowId: string, workflowJson: unknown): Promise<WorkflowPersistResponse>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(withWorkflowId(workflowId, workflowJson)),
  });
  ensureOk(response);
  return (await response.json()) as WorkflowPersistResponse;
}

export async function validateDraft(workflowJson: unknown): Promise<WorkflowValidationResponse>
{
  const response = await authFetch("/api/workflows/validate", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(workflowJson),
  });
  ensureOk(response);
  return (await response.json()) as WorkflowValidationResponse;
}

export async function validateWorkflow(workflowId: string): Promise<WorkflowValidationResponse>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}/validate`);
  ensureOk(response);
  return (await response.json()) as WorkflowValidationResponse;
}

export async function runWorkflow(workflowId: string): Promise<WorkflowRunResponse>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}/run`, {
    method: "POST",
  });
  ensureOk(response);
  return (await response.json()) as WorkflowRunResponse;
}

export async function deleteWorkflow(workflowId: string): Promise<WorkflowDeleteResponse>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}`, {
    method: "DELETE",
  });
  ensureOk(response);
  return (await response.json()) as WorkflowDeleteResponse;
}

export type CleanWorkflowResponse = {
  ok: boolean;
  workflowId: string;
  deletedFiles?: number;
  deletedDirs?: number;
  errors?: string[];
};

export async function cleanWorkflow(workflowId: string): Promise<CleanWorkflowResponse>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}/clean`, {
    method: "DELETE",
  });
  ensureOk(response);
  return (await response.json()) as CleanWorkflowResponse;
}

export type CancelRunResponse = {
  ok: boolean;
  runId: string;
  cancelRequested: boolean;
};

export async function cancelRun(runId: string): Promise<CancelRunResponse>
{
  const response = await authFetch(`/api/workflow-runs/${encodeURIComponent(runId)}/cancel`, {
    method: "POST",
  });
  ensureOk(response);
  return (await response.json()) as CancelRunResponse;
}

export type PauseRunResponse = { ok: boolean; runId: string; paused: boolean };

export async function pauseRun(runId: string): Promise<PauseRunResponse>
{
  const response = await authFetch(`/api/workflow-runs/${encodeURIComponent(runId)}/pause`, {
    method: "POST",
  });
  ensureOk(response);
  return (await response.json()) as PauseRunResponse;
}

export type ResumeRunResponse = { ok: boolean; runId: string; resumed: boolean };

export async function resumeRun(runId: string): Promise<ResumeRunResponse>
{
  const response = await authFetch(`/api/workflow-runs/${encodeURIComponent(runId)}/resume`, {
    method: "POST",
  });
  ensureOk(response);
  return (await response.json()) as ResumeRunResponse;
}

export type StopRunResponse = { ok: boolean; runId: string; stopRequested: boolean };

export async function stopRun(runId: string): Promise<StopRunResponse>
{
  const response = await authFetch(`/api/workflow-runs/${encodeURIComponent(runId)}/stop`, {
    method: "POST",
  });
  ensureOk(response);
  return (await response.json()) as StopRunResponse;
}

export type RunDetailTask = {
  taskId: string;
  state: string;
  attemptCount?: number;
  error?: string;
  startedAt?: string;
  completedAt?: string;
  capturedStdout?: string;
  capturedStderr?: string;
};

export type RunDetailResponse = {
  ok: boolean;
  run: {
    runId: string;
    workflowId: string;
    state: string;
    startedAt?: string;
    completedAt?: string;
    tasks: RunDetailTask[];
  };
};

export type ScriptCheckResponse = {
  ok: boolean;
  path: string;
  exists: boolean;
  executable: boolean;
  error?: string;
  message?: string;
};

export async function checkScript(scriptPath: string): Promise<ScriptCheckResponse>
{
  const response = await authFetch(`/api/scripts/check?path=${encodeURIComponent(scriptPath)}`);
  return (await response.json()) as ScriptCheckResponse;
}

export type FileCheckResponse = {
  ok: boolean;
  path: string;
  exists: boolean;
  error?: string;
  message?: string;
};

export async function checkFileExists(filePath: string, workflowId?: string, wd?: string): Promise<FileCheckResponse>
{
  let url = `/api/files/check?path=${encodeURIComponent(filePath)}`;
  if (workflowId) url += `&workflowId=${encodeURIComponent(workflowId)}`;
  if (wd) url += `&wd=${encodeURIComponent(wd)}`;
  const response = await authFetch(url);
  return (await response.json()) as FileCheckResponse;
}

export async function fetchRunDetails(runId: string): Promise<RunDetailResponse>
{
  const response = await authFetch(`/api/workflow-runs/${encodeURIComponent(runId)}`);
  ensureOk(response);
  return (await response.json()) as RunDetailResponse;
}
