import { useEffect, useState, useCallback } from "react";
import type { StatusResponse, WorkflowEntry } from "../types";
import { fetchStatus, fetchWorkflows } from "../api";

interface PollingState {
  status: StatusResponse | null;
  workflows: WorkflowEntry[];
}

export function usePolling(intervalMs: number = 3000) {
  const [state, setState] = useState<PollingState>({
    status: null,
    workflows: [],
  });

  const poll = useCallback(async () => {
    try {
      const [status, wfResponse] = await Promise.all([
        fetchStatus(),
        fetchWorkflows(),
      ]);
      setState({
        status,
        workflows: wfResponse.workflows ?? [],
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
