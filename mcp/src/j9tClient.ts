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
