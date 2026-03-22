export type VersionEntry = {
  timestamp: string;
  sizeBytes?: number;
};

export type VersionsListResponse = {
  ok: boolean;
  workflowId: string;
  versions: VersionEntry[];
  count: number;
};

export type VersionRestoreResponse = {
  ok: boolean;
  workflowId: string;
  restoredVersion: string;
};

export async function listVersions(workflowId: string): Promise<VersionsListResponse>
{
  const response = await fetch(`/api/workflows/${encodeURIComponent(workflowId)}/versions`);
  return (await response.json()) as VersionsListResponse;
}

export async function getVersionContent(workflowId: string, timestamp: string): Promise<string>
{
  const response = await fetch(
    `/api/workflows/${encodeURIComponent(workflowId)}/versions/${encodeURIComponent(timestamp)}`
  );
  return await response.text();
}

export async function restoreVersion(
  workflowId: string,
  timestamp: string
): Promise<VersionRestoreResponse>
{
  const response = await fetch(
    `/api/workflows/${encodeURIComponent(workflowId)}/versions/${encodeURIComponent(timestamp)}/restore`,
    { method: "POST" }
  );
  return (await response.json()) as VersionRestoreResponse;
}
