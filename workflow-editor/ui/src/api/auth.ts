// whoami probe — used for RBAC-aware UI gating.
// Studio returns {user: "studio", role: "admin"} when no MCP key is attached,
// so the editor defaults to full permissions when the call succeeds without auth.

export interface WhoamiResponse {
  ok: boolean;
  user?: string;
  role?: "admin" | "operator" | "viewer";
  error?: string;
}

export async function whoami(): Promise<WhoamiResponse | null>
{
  try
  {
    const res = await fetch("/api/auth/whoami", { credentials: "same-origin" });
    if (!res.ok) return null;
    return (await res.json()) as WhoamiResponse;
  }
  catch
  {
    return null;
  }
}
