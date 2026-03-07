import { useEffect, useState, useCallback } from "react";
import type { StatusResponse, WorkflowEntry } from "../types";
import { fetchStatus, fetchWorkflows, fetchKeysStatus } from "../api";

interface PollingState {
  status: StatusResponse | null;
  workflows: WorkflowEntry[];
  hasProviders: boolean;
}

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
        fetchWorkflows(),
        fetchKeysStatus(),
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
