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

async function ensureOk(response: Response): Promise<void>
{
  if (!response.ok)
  {
    // Surface the backend's JSON error body (MakeWorkflowJsonError → { code, message }) so the
    // real reason reaches the user instead of a bare "400 Bad Request".
    let detail = "";
    try
    {
      const body = await response.clone().json() as { code?: string; message?: string; error?: string };
      const parts = [body.code, body.message ?? body.error].filter((p): p is string => typeof p === "string" && p.length > 0);
      detail = parts.join(": ");
    }
    catch
    {
      // body wasn't JSON — fall back to status text
    }
    throw new Error(detail ? `HTTP ${response.status}: ${detail}` : `HTTP ${response.status} ${response.statusText}`);
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
  await ensureOk(response);
}

export async function listWorkflows(): Promise<WorkflowListResponse>
{
  const response = await authFetch("/api/workflows");
  await ensureOk(response);
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
  await ensureOk(response);
  return await response.json();
}

export async function createWorkflow(workflowJson: unknown): Promise<WorkflowPersistResponse>
{
  const response = await authFetch("/api/workflows", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(workflowJson),
  });
  await ensureOk(response);
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
  await ensureOk(response);
  return (await response.json()) as WorkflowPersistResponse;
}

export async function validateDraft(workflowJson: unknown): Promise<WorkflowValidationResponse>
{
  const response = await authFetch("/api/workflows/validate", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(workflowJson),
  });
  await ensureOk(response);
  return (await response.json()) as WorkflowValidationResponse;
}

export async function validateWorkflow(workflowId: string): Promise<WorkflowValidationResponse>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}/validate`);
  await ensureOk(response);
  return (await response.json()) as WorkflowValidationResponse;
}

export async function runWorkflow(workflowId: string): Promise<WorkflowRunResponse>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}/run`, {
    method: "POST",
  });
  await ensureOk(response);
  return (await response.json()) as WorkflowRunResponse;
}

export async function deleteWorkflow(workflowId: string): Promise<WorkflowDeleteResponse>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}`, {
    method: "DELETE",
  });
  await ensureOk(response);
  return (await response.json()) as WorkflowDeleteResponse;
}

// Artifact files in a workflow's folder (U3 / editor file nodes).
export type WorkflowFileEntry = {
  path: string;
  is_dir: boolean;
  size_bytes: number;
  modified_at?: string;
};

export async function listWorkflowFiles(workflowId: string): Promise<WorkflowFileEntry[]>
{
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}/files`);
  await ensureOk(response);
  const body = (await response.json()) as { files?: WorkflowFileEntry[] };
  return body.files ?? [];
}

/**
 * POST /api/workflows/{id}/files — upload one file (multipart, field "file") into the workflow
 * folder.  Let the browser set the multipart Content-Type + boundary (do NOT set it manually).
 * Returns the stored basename + size.
 */
export async function uploadWorkflowFile(workflowId: string, file: File): Promise<{ path: string; size_bytes: number }>
{
  const form = new FormData();
  form.append("file", file, file.name);
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}/files`, {
    method: "POST",
    body: form,
  });
  await ensureOk(response);
  return (await response.json()) as { path: string; size_bytes: number };
}

/**
 * GET /api/workflows/{id}/files/{path} — read one file's text content from the workflow folder
 * (e.g. a filter's source CSV, so the fan-out builder can parse its header). Returns the raw text.
 */
export async function getWorkflowFile(workflowId: string, relPath: string): Promise<string>
{
  const encodedPath = relPath.split("/").map(encodeURIComponent).join("/");
  const response = await authFetch(`/api/workflows/${encodeURIComponent(workflowId)}/files/${encodedPath}`);
  await ensureOk(response);
  return await response.text();
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
  await ensureOk(response);
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
  await ensureOk(response);
  return (await response.json()) as CancelRunResponse;
}

export type PauseRunResponse = { ok: boolean; runId: string; paused: boolean };

export async function pauseRun(runId: string): Promise<PauseRunResponse>
{
  const response = await authFetch(`/api/workflow-runs/${encodeURIComponent(runId)}/pause`, {
    method: "POST",
  });
  await ensureOk(response);
  return (await response.json()) as PauseRunResponse;
}

export type ResumeRunResponse = { ok: boolean; runId: string; resumed: boolean };

export async function resumeRun(runId: string): Promise<ResumeRunResponse>
{
  const response = await authFetch(`/api/workflow-runs/${encodeURIComponent(runId)}/resume`, {
    method: "POST",
  });
  await ensureOk(response);
  return (await response.json()) as ResumeRunResponse;
}

export type StopRunResponse = { ok: boolean; runId: string; stopRequested: boolean };

export async function stopRun(runId: string): Promise<StopRunResponse>
{
  const response = await authFetch(`/api/workflow-runs/${encodeURIComponent(runId)}/stop`, {
    method: "POST",
  });
  await ensureOk(response);
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
  await ensureOk(response);
  return (await response.json()) as RunDetailResponse;
}
