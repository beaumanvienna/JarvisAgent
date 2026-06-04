// j9t MCP Server configuration.
// Reads from environment variables with sensible defaults for local development.

import { readFileSync } from "node:fs";

export interface J9tConfig
{
    /** Base URL of the j9t REST API (e.g., "http://localhost:8080") */
    baseUrl: string;
    /** Bearer token for authenticating with j9t Engine */
    token: string;
}

function readTokenFromFile(path: string): string
{
    try
    {
        return readFileSync(path, "utf-8").trim();
    }
    catch
    {
        return "";
    }
}

export function loadConfig(): J9tConfig
{
    const baseUrl = process.env.J9T_URL ?? "http://localhost:8080";

    let token = process.env.J9T_TOKEN ?? "";
    if (!token && process.env.J9T_TOKEN_FILE)
    {
        token = readTokenFromFile(process.env.J9T_TOKEN_FILE);
    }

    return { baseUrl, token };
}
