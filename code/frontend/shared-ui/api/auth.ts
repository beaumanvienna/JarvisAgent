// j9t auth helpers — cookie-based only.
// The only supported auth paths are:
//   1. Session cookie set by POST /api/auth/login (browser flow).
//   2. Gateway headers (X-Forwarded-User/Role) forwarded by a reverse proxy.
// No bearer token in localStorage — the cyber-security story stays one paragraph:
// "session cookie, or gateway header".

export async function authFetch(
  url: string,
  init?: RequestInit
): Promise<Response> {
  // `same-origin` is the default but setting it explicitly documents intent:
  // the session cookie set by /api/auth/login must travel on every API call.
  const res = await fetch(url, { credentials: "same-origin", ...init });
  if (res.status === 401 || res.status === 403) {
    window.dispatchEvent(new Event("j9t-auth-required"));
  }
  return res;
}

/** POST /api/auth/logout — destroys the server-side session and clears the cookie. */
export async function serverLogout(): Promise<void> {
  try {
    await fetch(`${window.location.origin}/api/auth/logout`, {
      method: "POST",
      credentials: "same-origin",
    });
  } catch {
    // Best effort — the server may already be gone.
  }
}

export interface WhoamiResponse {
  ok: boolean;
  user?: string;
  role?: "admin" | "operator" | "viewer";
  error?: string;
}

export async function whoami(): Promise<WhoamiResponse | null> {
  try {
    const res = await authFetch(`${window.location.origin}/api/auth/whoami`);
    if (!res.ok) return null;
    return (await res.json()) as WhoamiResponse;
  } catch {
    return null;
  }
}
