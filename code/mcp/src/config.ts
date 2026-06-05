// j9t MCP Server configuration.
// Reads from environment variables with sensible defaults for local development.

import { readFileSync } from "node:fs";

export interface J9tConfig
{
    /** Base URL of the j9t REST API (e.g., "https://localhost:8443") */
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
    // A default j9t serves HTTPS on 8443 (it mints its own self-signed localhost
    // cert on first start). Point NODE_EXTRA_CA_CERTS at certs/j9t-cert.pem in the
    // j9t working directory so Node's fetch trusts it. For a plain-HTTP j9t (e.g.
    // Docker's default on 8080), set J9T_URL explicitly.
    const baseUrl = process.env.J9T_URL ?? "https://localhost:8443";

    let token = process.env.J9T_TOKEN ?? "";
    if (!token && process.env.J9T_TOKEN_FILE)
    {
        token = readTokenFromFile(process.env.J9T_TOKEN_FILE);
    }

    return { baseUrl, token };
}
