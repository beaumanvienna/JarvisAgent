const STORAGE_KEY = "j9t_admin_token";

export function getToken(): string | null {
  return localStorage.getItem(STORAGE_KEY);
}

export function setToken(token: string): void {
  localStorage.setItem(STORAGE_KEY, token);
}

export function clearToken(): void {
  localStorage.removeItem(STORAGE_KEY);
}

export function authHeaders(): Record<string, string> {
  const token = getToken();
  if (token) {
    return { Authorization: `Bearer ${token}` };
  }
  return {};
}

/** Fetch wrapper that adds auth headers and detects 401/403. */
export async function authFetch(
  url: string,
  init?: RequestInit
): Promise<Response> {
  const headers = { ...authHeaders(), ...((init?.headers as Record<string, string>) || {}) };
  const res = await fetch(url, { ...init, headers });
  if (res.status === 401 || res.status === 403) {
    clearToken();
    window.dispatchEvent(new Event("j9t-auth-required"));
  }
  return res;
}
