import React, { useEffect, useMemo, useState } from "react";
import { listWorkflows, type WorkflowListResponse } from "../api/workflows";

export type WorkflowListItem = {
  id: string;
  label?: string;
  path?: string;
};

export default function WorkflowListView(props: {
  onOpenWorkflow: (workflow: WorkflowListItem) => void;
}): JSX.Element
{
  const [loading, setLoading] = useState<boolean>(true);
  const [error, setError] = useState<string | null>(null);
  const [response, setResponse] = useState<WorkflowListResponse | null>(null);

  useEffect(() => {
    let isCancelled = false;

    async function load(): Promise<void>
    {
      try
      {
        setLoading(true);
        setError(null);

        const data = await listWorkflows();
        if (!isCancelled)
        {
          setResponse(data);
        }
      }
      catch (e)
      {
        if (!isCancelled)
        {
          const message = e instanceof Error ? e.message : String(e);
          setError(message);
        }
      }
      finally
      {
        if (!isCancelled)
        {
          setLoading(false);
        }
      }
    }

    void load();

    return () => { isCancelled = true; };
  }, []);

  const workflows = useMemo(() => {
    return response?.workflows ?? [];
  }, [response]);

  return (
    <div className="panel">
      <h2 style={{ marginTop: 0 }}>Workflows</h2>

      {loading ? <div className="muted">Loading…</div> : null}
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
