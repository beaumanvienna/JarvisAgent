import { useEffect, useState, useCallback } from "react";
import type { StatusResponse, WorkflowEntry } from "../types";
import { fetchStatus, fetchWorkflows, fetchKeysStatus } from "../api";

interface PollingState {
  status: StatusResponse | null;
  workflows: WorkflowEntry[];
  hasProviders: boolean;
}

const KEYS_DEFAULT = { ok: true, has_providers: true, status: "ok", message: "" };

export function usePolling(intervalMs: number = 3000) {
  const [state, setState] = useState<PollingState>({
    status: null,
    workflows: [],
    hasProviders: true,
  });

  const poll = useCallback(async () => {
    try {
      const [status, wfResponse, keysResponse] = await Promise.all([
        fetchStatus(),
        // Workflows endpoint requires auth in Engine mode; default to empty on 401
        fetchWorkflows().catch(() => ({ workflows: [] })),
        // Keys endpoint is Studio-only; gracefully default in Engine mode
        fetchKeysStatus().catch(() => KEYS_DEFAULT),
      ]);
      setState({
        status,
        workflows: wfResponse.workflows ?? [],
        hasProviders: keysResponse.has_providers ?? true,
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
