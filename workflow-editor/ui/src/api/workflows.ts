export type WorkflowListItem = {
  id: string;
  label?: string;
  path?: string;
};

export type WorkflowListResponse = {
  workflows: WorkflowListItem[];
};

export type WorkflowValidationFinding = {
  code: string;
  message: string;
};

export type WorkflowValidationResponse = {
  ok: boolean;
  id: string;
  errors: WorkflowValidationFinding[];
  warnings: WorkflowValidationFinding[];
};

function ensureOk(response: Response): void
{
  if (!response.ok)
  {
    throw new Error(`HTTP ${response.status} ${response.statusText}`);
  }
}

export async function listWorkflows(): Promise<WorkflowListResponse>
{
  const response = await fetch("/api/workflows");
  ensureOk(response);
  return (await response.json()) as WorkflowListResponse;
}

/**
 * GET /api/workflows/{id}
 * Backend returns the raw JCWF JSON body (canonical) as the response.
 */
export async function loadWorkflow(workflowId: string): Promise<unknown>
{
  const response = await fetch(`/api/workflows/${encodeURIComponent(workflowId)}`);
  ensureOk(response);
  return await response.json();
}

export async function createWorkflow(workflowJson: unknown): Promise<{ ok: boolean; id: string; savedPath?: string; }>
{
  const response = await fetch("/api/workflows", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(workflowJson),
  });
  ensureOk(response);
  return (await response.json()) as { ok: boolean; id: string; savedPath?: string; };
}

export async function saveWorkflow(workflowId: string, workflowJson: unknown): Promise<{ ok: boolean; id: string; savedPath?: string; }>
{
  const response = await fetch(`/api/workflows/${encodeURIComponent(workflowId)}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(workflowJson),
  });
  ensureOk(response);
  return (await response.json()) as { ok: boolean; id: string; savedPath?: string; };
}

export async function validateDraft(workflowJson: unknown): Promise<WorkflowValidationResponse>
{
  const response = await fetch("/api/workflows/validate", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(workflowJson),
  });
  ensureOk(response);
  return (await response.json()) as WorkflowValidationResponse;
}

export async function validateWorkflow(workflowId: string): Promise<WorkflowValidationResponse>
{
  const response = await fetch(`/api/workflows/${encodeURIComponent(workflowId)}/validate`);
  ensureOk(response);
  return (await response.json()) as WorkflowValidationResponse;
}

export async function runWorkflow(workflowId: string): Promise<{ ok: boolean; id: string; runId: string; enqueued: boolean; }>
{
  const response = await fetch(`/api/workflows/${encodeURIComponent(workflowId)}/run`, {
    method: "POST",
  });
  ensureOk(response);
  return (await response.json()) as { ok: boolean; id: string; runId: string; enqueued: boolean; };
}
