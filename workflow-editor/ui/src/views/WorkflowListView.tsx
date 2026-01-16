import React, { useCallback, useEffect, useMemo, useState } from "react";
import { deleteWorkflow, listWorkflows, type WorkflowListResponse } from "../api/workflows";

export type WorkflowListItem = {
  id: string;
  label?: string;
  path?: string;
};

export default function WorkflowListView(props: {
  refreshToken: number;
  onOpenWorkflow: (workflow: WorkflowListItem) => void;
  onCreateNew: () => void;
}): JSX.Element
{
  const [loading, setLoading] = useState<boolean>(true);
  const [error, setError] = useState<string | null>(null);
  const [statusText, setStatusText] = useState<string>("");
  const [response, setResponse] = useState<WorkflowListResponse | null>(null);

  const reload = useCallback(async (isCancelled: { value: boolean }): Promise<void> => {
    try
    {
      setLoading(true);
      setError(null);
      const data = await listWorkflows();
      if (!isCancelled.value)
      {
        setResponse(data);
      }
    }
    catch (e)
    {
      if (!isCancelled.value)
      {
        const message = e instanceof Error ? e.message : String(e);
        setError(message);
      }
    }
    finally
    {
      if (!isCancelled.value)
      {
        setLoading(false);
      }
    }
  }, []);

  // Re-load list when entering the list view OR when the parent signals that
  // something changed (create/save-as/delete).
  useEffect(() => {
    const isCancelled = { value: false };
    void reload(isCancelled);
    return () => { isCancelled.value = true; };
  }, [reload, props.refreshToken]);

  const onDelete = useCallback(async (workflowId: string) => {
    const confirmed = window.confirm(`Delete workflow '${workflowId}'?`);
    if (!confirmed)
    {
      return;
    }

    const isCancelled = { value: false };
    try
    {
      setError(null);
      setStatusText("Deleting…");
      await deleteWorkflow(workflowId);
      setStatusText(`Deleted '${workflowId}'.`);
      await reload(isCancelled);
    }
    catch (e)
    {
      const message = e instanceof Error ? e.message : String(e);
      setError(`Delete failed: ${message}`);
      setStatusText("");
    }
  }, [reload]);

  const workflows = useMemo(() => {
    return response?.workflows ?? [];
  }, [response]);

  return (
    <div className="panel">
      <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: 12 }}>
        <h2 style={{ marginTop: 0, marginBottom: 0 }}>Workflows</h2>
        <div style={{ display: "flex", gap: 8 }}>
          <button className="btn" type="button" onClick={props.onCreateNew}>
            + New
          </button>
          <button className="btn" type="button" onClick={() => {
            const isCancelled = { value: false };
            void reload(isCancelled);
          }} disabled={loading}>
            Refresh
          </button>
        </div>
      </div>

      {loading ? <div className="muted">Loading…</div> : null}
      {statusText ? <div className="small">{statusText}</div> : null}
      {error ? <div className="errorText">Error: {error}</div> : null}

      {!loading && !error && workflows.length === 0
        ? <div className="muted">No workflows found.</div>
        : null}

      <div style={{ maxWidth: 860 }}>
        {workflows.map((w) => {
          const item: WorkflowListItem = { id: w.id, label: w.label, path: w.path };
          return (
            <div key={w.id} className="card" style={{ display: "flex", gap: 12, alignItems: "center", justifyContent: "space-between" }}>
              <div>
                <div style={{ fontWeight: 700 }}>{w.label && w.label.length > 0 ? w.label : w.id}</div>
                <div className="small">{w.path ? w.path : "path unknown"}</div>
              </div>

              <div style={{ display: "flex", gap: 8 }}>
                <button className="btn" type="button" onClick={() => { props.onOpenWorkflow(item); }}>
                  Open
                </button>
                <button className="btn" type="button" onClick={() => { void onDelete(w.id); }}>
                  Delete
                </button>
              </div>
            </div>
          );
        })}
      </div>

      <div className="card" style={{ maxWidth: 860 }}>
        <div style={{ fontWeight: 700, marginBottom: 6 }}>Tip</div>
        <div className="small">
          Workflows are loaded from JarvisAgent via <code>/api/workflows</code>.
          The editor will load the selected workflow by id.
        </div>
      </div>
    </div>
  );
}
