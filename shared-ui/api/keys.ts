import { authFetch } from "@shared/api/auth";

export type KeysStatusResponse = {
  ok: boolean;
  status: "ok" | "no_password" | "wrong_password" | "no_keys_file";
  message: string;
  has_providers: boolean;
};

export interface IssuedAdminKey {
  key_id: string;
  api_key: string;
  user: string;
  role: string;
  expires_at: string;
}

export interface KeysUnlockResponse {
  ok: boolean;
  status?: string;
  message?: string;
  error?: string;
  bootstrapped?: boolean;
  mcp_keys_loaded?: boolean;
  // Populated when the MCP key store was empty after unlock — the server has
  // created a fresh admin key and handed it back so the UI can display it
  // without the user fishing an enrollment token out of log/log.txt.
  admin_key?: IssuedAdminKey;
}

export async function getKeysStatus(): Promise<KeysStatusResponse>
{
  const response = await authFetch("/api/settings/keys/status");
  if (!response.ok)
  {
    throw new Error(`HTTP ${response.status} ${response.statusText}`);
  }
  return (await response.json()) as KeysStatusResponse;
}

export async function unlockKeys(masterPassword: string): Promise<KeysUnlockResponse>
{
  const response = await authFetch("/api/settings/keys/unlock", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ master_password: masterPassword }),
  });
  return (await response.json()) as KeysUnlockResponse;
}
