export type KeysStatusResponse = {
  ok: boolean;
  status: "ok" | "no_password" | "wrong_password" | "no_keys_file";
  message: string;
  has_providers: boolean;
};

export type KeysUnlockResponse = {
  ok: boolean;
  status?: string;
  message?: string;
  error?: string;
};

export async function getKeysStatus(): Promise<KeysStatusResponse>
{
  const response = await fetch("/api/settings/keys/status");
  if (!response.ok)
  {
    throw new Error(`HTTP ${response.status} ${response.statusText}`);
  }
  return (await response.json()) as KeysStatusResponse;
}

export async function unlockKeys(masterPassword: string): Promise<KeysUnlockResponse>
{
  const response = await fetch("/api/settings/keys/unlock", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ master_password: masterPassword }),
  });
  return (await response.json()) as KeysUnlockResponse;
}
