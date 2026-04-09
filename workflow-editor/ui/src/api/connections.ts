export type ConnectionEntry = {
  name: string;
  type: string;
  endpoint: string;
  key_name: string;
  auth_type: string;
  params: Record<string, string>;
};

export type ConnectionsListResponse = {
  ok: boolean;
  dirty: boolean;
  connections: ConnectionEntry[];
};

export type ConnectionMutationResponse = {
  ok: boolean;
  name?: string;
  error?: string;
  message?: string;
};

export type ConnectionTestResponse = {
  ok: boolean;
  error?: string;
  message?: string;
};

export type ConnectionSaveResponse = {
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

export async function listConnections(): Promise<ConnectionsListResponse>
{
  const response = await fetch("/api/connections");
  ensureOk(response);
  return (await response.json()) as ConnectionsListResponse;
}

export type ConnectionCreateInput = {
  name: string;
  type?: string;
  endpoint?: string;
  key_name?: string;
  auth_type?: string;
  params?: Record<string, string>;
};

export async function createConnection(input: ConnectionCreateInput): Promise<ConnectionMutationResponse>
{
  const response = await fetch("/api/connections", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(input),
  });
  return (await response.json()) as ConnectionMutationResponse;
}

export type ConnectionUpdateInput = {
  type?: string;
  endpoint?: string;
  key_name?: string;
  auth_type?: string;
  params?: Record<string, string>;
};

export async function updateConnection(name: string, input: ConnectionUpdateInput): Promise<ConnectionMutationResponse>
{
  const response = await fetch(`/api/connections/${encodeURIComponent(name)}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(input),
  });
  return (await response.json()) as ConnectionMutationResponse;
}

export async function deleteConnection(name: string): Promise<ConnectionMutationResponse>
{
  const response = await fetch(`/api/connections/${encodeURIComponent(name)}`, {
    method: "DELETE",
  });
  return (await response.json()) as ConnectionMutationResponse;
}

export async function testConnection(name: string): Promise<ConnectionTestResponse>
{
  const response = await fetch(`/api/connections/${encodeURIComponent(name)}/test`, {
    method: "POST",
  });
  return (await response.json()) as ConnectionTestResponse;
}

export async function saveConnections(): Promise<ConnectionSaveResponse>
{
  const response = await fetch("/api/connections/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
  });
  return (await response.json()) as ConnectionSaveResponse;
}
