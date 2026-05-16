import { useEffect, useState, useCallback } from "react";
import type { StatusResponse, WorkflowEntry, ProviderHealth } from "../types";
import { fetchStatus, fetchWorkflows, fetchKeysStatus, fetchProvidersHealth } from "../api";

interface PollingState {
  status: StatusResponse | null;
  workflows: WorkflowEntry[];
  hasProviders: boolean;
  providersHealth: ProviderHealth[];
}

const KEYS_DEFAULT = { ok: true, has_providers: true, status: "ok", message: "" };

export function usePolling(intervalMs: number = 3000) {
  const [state, setState] = useState<PollingState>({
    status: null,
    workflows: [],
    hasProviders: true,
    providersHealth: [],
  });

  const poll = useCallback(async () => {
    try {
      const [status, wfResponse, keysResponse, healthResponse] = await Promise.all([
        fetchStatus(),
        // Workflows endpoint requires auth in Engine mode; default to empty on 401
        fetchWorkflows().catch(() => ({ workflows: [] })),
        // Keys endpoint is Studio-only; gracefully default in Engine mode
        fetchKeysStatus().catch(() => KEYS_DEFAULT),
        // Providers health endpoint — Sitting 8.  Same auth posture as workflows;
        // default to empty interfaces array on 401 / failure so the LED renders grey.
        fetchProvidersHealth().catch(() => ({ ok: false, interfaces: [] as ProviderHealth[] })),
      ]);
      setState({
        status,
        workflows: wfResponse.workflows ?? [],
        hasProviders: keysResponse.has_providers ?? true,
        providersHealth: healthResponse.interfaces ?? [],
      });
    } catch {
      setState((prev) => ({ ...prev, status: null }));
    }
  }, []);

  useEffect(() => {
    poll();
    const id = setInterval(poll, intervalMs);
    return () => clearInterval(id);
  }, [poll, intervalMs]);

  return { ...state, refresh: poll };
}
