// HTTP client for the j9t REST API.
// All MCP tools and resources proxy through this.

import type { J9tConfig } from "./config.js";

export class J9tClient
{
    private baseUrl: string;
    private headers: Record<string, string>;

    constructor(config: J9tConfig)
    {
        // Strip trailing slash
        this.baseUrl = config.baseUrl.replace(/\/+$/, "");
        this.headers = {
            "Content-Type": "application/json",
        };
        if (config.token)
        {
            this.headers["Authorization"] = `Bearer ${config.token}`;
        }
    }

    async get<T = unknown>(path: string): Promise<T>
    {
        const url = `${this.baseUrl}${path}`;
        const response = await fetch(url, { method: "GET", headers: this.headers });
        if (!response.ok)
        {
            const body = await response.text();
            throw new Error(`GET ${path} failed: ${response.status} ${response.statusText} — ${body}`);
        }
        return (await response.json()) as T;
    }

    async post<T = unknown>(path: string, body?: unknown): Promise<T>
    {
        const url = `${this.baseUrl}${path}`;
        const response = await fetch(url, {
            method: "POST",
            headers: this.headers,
            body: body !== undefined ? JSON.stringify(body) : undefined,
        });
        if (!response.ok)
        {
            const text = await response.text();
            throw new Error(`POST ${path} failed: ${response.status} ${response.statusText} — ${text}`);
        }
        return (await response.json()) as T;
    }

    async put<T = unknown>(path: string, body?: unknown): Promise<T>
    {
        const url = `${this.baseUrl}${path}`;
        const response = await fetch(url, {
            method: "PUT",
            headers: this.headers,
            body: body !== undefined ? JSON.stringify(body) : undefined,
        });
        if (!response.ok)
        {
            const text = await response.text();
            throw new Error(`PUT ${path} failed: ${response.status} ${response.statusText} — ${text}`);
        }
        return (await response.json()) as T;
    }

    async delete<T = unknown>(path: string): Promise<T>
    {
        const url = `${this.baseUrl}${path}`;
        const response = await fetch(url, { method: "DELETE", headers: this.headers });
        if (!response.ok)
        {
            const text = await response.text();
            throw new Error(`DELETE ${path} failed: ${response.status} ${response.statusText} — ${text}`);
        }
        return (await response.json()) as T;
    }

    /**
     * Raw fetch — returns the underlying Response so callers can inspect status,
     * headers, and body format (text vs binary). Used by get_run_file where we
     * need Content-Type + X-Content-SHA256 + Range semantics.
     */
    async fetchRaw(path: string, init: RequestInit = {}): Promise<Response>
    {
        const url = `${this.baseUrl}${path}`;
        const mergedHeaders: Record<string, string> = { ...this.headers };
        if (init.headers)
        {
            for (const [k, v] of Object.entries(init.headers as Record<string, string>))
            {
                mergedHeaders[k] = v;
            }
        }
        return await fetch(url, { ...init, method: init.method ?? "GET", headers: mergedHeaders });
    }

    /** Raw text fetch for endpoints that return non-JSON bodies (e.g. log tails). */
    async getText(path: string): Promise<string>
    {
        const url = `${this.baseUrl}${path}`;
        const response = await fetch(url, { method: "GET", headers: this.headers });
        if (!response.ok)
        {
            const body = await response.text();
            throw new Error(`GET ${path} failed: ${response.status} ${response.statusText} — ${body}`);
        }
        return await response.text();
    }

    /** Health check — returns true if j9t is reachable. */
    async ping(): Promise<boolean>
    {
        try
        {
            await this.get("/api/status");
            return true;
        }
        catch
        {
            return false;
        }
    }
}
