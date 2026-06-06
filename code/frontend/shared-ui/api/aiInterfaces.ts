import { authFetch } from "@shared/api/auth";

export type RequestBudgetConfig = {
  per_1k_input_token_seconds?: number;
  per_1k_output_token_seconds?: number;
  fixed_overhead_seconds?: number;
  safety_margin_factor?: number;
  min_seconds?: number;
  max_seconds?: number;
};

export type RateLimitConfig = {
  initial_concurrency_probe?: number;
  max_concurrency?: number;
  max_retries_429?: number;
  max_retries_transient?: number;
  base_retry_ms?: number;
  request_budget?: RequestBudgetConfig;
};

export type AiInterface = {
  name: string;
  description: string;
  url: string;
  model: string;
  api_type: string;
  key_name: string;
  max_context_tokens?: number;
  default_output_tokens?: number;
  rate_limit?: RateLimitConfig;
};

export type AiInterfacesListResponse = {
  ok: boolean;
  api_index: number;
  dirty: boolean;
  interfaces: AiInterface[];
};

export type AiInterfaceMutationResponse = {
  ok: boolean;
  name?: string;
  error?: string;
  message?: string;
};

export type AiInterfacesSaveResponse = {
  ok: boolean;
  path?: string;
  error?: string;
  message?: string;
};

export type ConfigReloadResponse = {
  ok: boolean;
  interface_count?: number;
  error?: string;
  message?: string;
};

function ensureOk(response: Response): void
{
  if (!response.ok)
  {
    throw new Error(`HTTP ${response.status} ${response.statusText}`);
  }
}

export async function listAiInterfaces(): Promise<AiInterfacesListResponse>
{
  const response = await authFetch("/api/settings/ai-interfaces");
  ensureOk(response);
  return (await response.json()) as AiInterfacesListResponse;
}

export type AiInterfaceCreateInput = {
  url: string;
  model?: string;
  api_type?: string;
  name?: string;
  description?: string;
  key_name?: string;
  max_context_tokens?: number;
  default_output_tokens?: number;
  rate_limit?: RateLimitConfig;
};

// All routing mutations require the master password re-supplied in the body
// (the backend re-auth gate, WebServer::CheckMasterPasswordReauth).  The caller
// obtains it from the MasterPasswordDialog confirm flow.  Optional at the type
// level only so the views can be wired incrementally; omitting it yields a
// 401 reauth_required from the server.
export async function createAiInterface(
  input: AiInterfaceCreateInput,
  masterPassword?: string,
): Promise<AiInterfaceMutationResponse>
{
  const response = await authFetch("/api/settings/ai-interfaces", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ...input, master_password: masterPassword }),
  });
  return (await response.json()) as AiInterfaceMutationResponse;
}

export type AiInterfaceUpdateInput = {
  url?: string;
  model?: string;
  api_type?: string;
  name?: string;
  description?: string;
  key_name?: string;
  max_context_tokens?: number;
  default_output_tokens?: number;
  rate_limit?: RateLimitConfig;
};

export async function updateAiInterface(
  name: string,
  input: AiInterfaceUpdateInput,
  masterPassword?: string,
): Promise<AiInterfaceMutationResponse>
{
  const response = await authFetch(`/api/settings/ai-interfaces/${encodeURIComponent(name)}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ...input, master_password: masterPassword }),
  });
  return (await response.json()) as AiInterfaceMutationResponse;
}

export async function deleteAiInterface(name: string, masterPassword?: string): Promise<AiInterfaceMutationResponse>
{
  const response = await authFetch(`/api/settings/ai-interfaces/${encodeURIComponent(name)}`, {
    method: "DELETE",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ master_password: masterPassword }),
  });
  return (await response.json()) as AiInterfaceMutationResponse;
}

export async function saveAiInterfaces(masterPassword?: string): Promise<AiInterfacesSaveResponse>
{
  const response = await authFetch("/api/settings/ai-interfaces/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ master_password: masterPassword }),
  });
  return (await response.json()) as AiInterfacesSaveResponse;
}

export type AiInterfaceTestResponse = {
  ok: boolean;
  index: number;
  name?: string;
  model?: string;
  latency_ms?: number;
  response_preview?: string;
  error?: string;
};

export async function testAiInterface(index: number): Promise<AiInterfaceTestResponse>
{
  const response = await authFetch("/api/settings/ai-interfaces/test", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ index }),
  });
  return (await response.json()) as AiInterfaceTestResponse;
}

export async function reloadConfig(): Promise<ConfigReloadResponse>
{
  const response = await authFetch("/api/settings/config/reload", {
    method: "POST",
  });
  return (await response.json()) as ConfigReloadResponse;
}
