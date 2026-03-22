export type AiInterface = {
  name: string;
  description: string;
  url: string;
  model: string;
  api_type: string;
  key_name: string;
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
  const response = await fetch("/api/settings/ai-interfaces");
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
};

export async function createAiInterface(input: AiInterfaceCreateInput): Promise<AiInterfaceMutationResponse>
{
  const response = await fetch("/api/settings/ai-interfaces", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(input),
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
};

export async function updateAiInterface(name: string, input: AiInterfaceUpdateInput): Promise<AiInterfaceMutationResponse>
{
  const response = await fetch(`/api/settings/ai-interfaces/${encodeURIComponent(name)}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(input),
  });
  return (await response.json()) as AiInterfaceMutationResponse;
}

export async function deleteAiInterface(name: string): Promise<AiInterfaceMutationResponse>
{
  const response = await fetch(`/api/settings/ai-interfaces/${encodeURIComponent(name)}`, {
    method: "DELETE",
  });
  return (await response.json()) as AiInterfaceMutationResponse;
}

export async function saveAiInterfaces(): Promise<AiInterfacesSaveResponse>
{
  const response = await fetch("/api/settings/ai-interfaces/save", {
    method: "POST",
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
  const response = await fetch("/api/settings/ai-interfaces/test", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ index }),
  });
  return (await response.json()) as AiInterfaceTestResponse;
}

export async function reloadConfig(): Promise<ConfigReloadResponse>
{
  const response = await fetch("/api/settings/config/reload", {
    method: "POST",
  });
  return (await response.json()) as ConfigReloadResponse;
}
