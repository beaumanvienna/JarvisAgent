export type ProviderEntry = {
  name: string;
  display_name: string;
  endpoint: string;
  default_model: string;
  api_type: string;
  has_key: boolean;
};

export type ProvidersListResponse = {
  ok: boolean;
  default_provider: string;
  providers: ProviderEntry[];
};

export type ProviderMutationResponse = {
  ok: boolean;
  name?: string;
  error?: string;
  message?: string;
};

export type ProviderSaveResponse = {
  ok: boolean;
  path?: string;
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

export async function listProviders(): Promise<ProvidersListResponse>
{
  const response = await fetch("/api/settings/providers");
  ensureOk(response);
  return (await response.json()) as ProvidersListResponse;
}

export type ProviderCreateInput = {
  name: string;
  display_name?: string;
  endpoint?: string;
  api_key?: string;
  default_model?: string;
  api_type?: string;
};

export async function createProvider(input: ProviderCreateInput): Promise<ProviderMutationResponse>
{
  const response = await fetch("/api/settings/providers", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(input),
  });
  return (await response.json()) as ProviderMutationResponse;
}

export type ProviderUpdateInput = {
  display_name?: string;
  endpoint?: string;
  api_key?: string;
  default_model?: string;
  api_type?: string;
};

export async function updateProvider(name: string, input: ProviderUpdateInput): Promise<ProviderMutationResponse>
{
  const response = await fetch(`/api/settings/providers/${encodeURIComponent(name)}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(input),
  });
  return (await response.json()) as ProviderMutationResponse;
}

export async function deleteProvider(name: string): Promise<ProviderMutationResponse>
{
  const response = await fetch(`/api/settings/providers/${encodeURIComponent(name)}`, {
    method: "DELETE",
  });
  return (await response.json()) as ProviderMutationResponse;
}

export async function setDefaultProvider(name: string): Promise<ProviderMutationResponse>
{
  const response = await fetch(`/api/settings/providers/${encodeURIComponent(name)}/default`, {
    method: "POST",
  });
  return (await response.json()) as ProviderMutationResponse;
}

export async function saveProviders(masterPassword?: string): Promise<ProviderSaveResponse>
{
  const body: Record<string, string> = {};
  if (masterPassword)
  {
    body.master_password = masterPassword;
  }

  const response = await fetch("/api/settings/providers/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  return (await response.json()) as ProviderSaveResponse;
}
