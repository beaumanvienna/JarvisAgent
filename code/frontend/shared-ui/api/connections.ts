import { authFetch } from "@shared/api/auth";

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
  const response = await authFetch("/api/connections");
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

// Connection mutations require the master password re-supplied in the body
// (backend re-auth gate).  Optional at the type level only so the views can be
// wired incrementally; omitting it yields a 401 reauth_required.
export async function createConnection(
  input: ConnectionCreateInput,
  masterPassword?: string,
): Promise<ConnectionMutationResponse>
{
  const response = await authFetch("/api/connections", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ...input, master_password: masterPassword }),
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

export async function updateConnection(
  name: string,
  input: ConnectionUpdateInput,
  masterPassword?: string,
): Promise<ConnectionMutationResponse>
{
  const response = await authFetch(`/api/connections/${encodeURIComponent(name)}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ...input, master_password: masterPassword }),
  });
  return (await response.json()) as ConnectionMutationResponse;
}

export async function deleteConnection(name: string, masterPassword?: string): Promise<ConnectionMutationResponse>
{
  const response = await authFetch(`/api/connections/${encodeURIComponent(name)}`, {
    method: "DELETE",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ master_password: masterPassword }),
  });
  return (await response.json()) as ConnectionMutationResponse;
}

export async function testConnection(name: string): Promise<ConnectionTestResponse>
{
  const response = await authFetch(`/api/connections/${encodeURIComponent(name)}/test`, {
    method: "POST",
  });
  return (await response.json()) as ConnectionTestResponse;
}

export async function saveConnections(masterPassword?: string): Promise<ConnectionSaveResponse>
{
  const response = await authFetch("/api/connections/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ master_password: masterPassword }),
  });
  return (await response.json()) as ConnectionSaveResponse;
}

export type OAuthAuthorizeResponse = {
  ok: boolean;
  authorize_url?: string;
  error?: string;
  message?: string;
};

export async function authorizeConnection(name: string): Promise<OAuthAuthorizeResponse>
{
  const response = await authFetch(`/api/connections/${encodeURIComponent(name)}/oauth/authorize`);
  return (await response.json()) as OAuthAuthorizeResponse;
}
