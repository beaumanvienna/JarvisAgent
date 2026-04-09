import React, { useCallback, useEffect, useMemo, useState } from "react";
import { deleteWorkflow, listWorkflows, reloadWorkflowRegistry, type WorkflowListResponse, type BrokenWorkflowItem } from "../api/workflows";
import CreateWorkflowModal from "../components/CreateWorkflowModal";
import TemplateBrowser from "../components/TemplateBrowser";
import type { JcwfFile } from "../jcwf/types";

export type WorkflowListItem = {
  id: string;
  label?: string;
  path?: string;
};

export default function WorkflowListView(props: {
  refreshToken: number;
  onOpenWorkflow: (workflow: WorkflowListItem) => void;
  onCreateNew: () => void;
  onCreateFromTemplate?: (workflowId: string, jcwf: JcwfFile) => void;
}): JSX.Element
{
  const [loading, setLoading] = useState<boolean>(true);
  const [error, setError] = useState<string | null>(null);
  const [statusText, setStatusText] = useState<string>("");
  const [response, setResponse] = useState<WorkflowListResponse | null>(null);
  const [filterText, setFilterText] = useState<string>("");
  const [showCreateModal, setShowCreateModal] = useState<boolean>(false);

  const reload = useCallback(async (isCancelled: { value: boolean }, triggerRegistryReload?: boolean): Promise<void> => {
    try
    {
      setLoading(true);
      setError(null);
      if (triggerRegistryReload)
      {
        await reloadWorkflowRegistry();
      }
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
    void reload(isCancelled, true);
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
    const all = (response?.workflows ?? []).filter((w) => !w.is_sub_workflow);
    if (filterText.trim().length === 0)
    {
      return all;
    }
    const lowerFilter = filterText.toLowerCase();
    return all.filter((w) => {
      const idMatch = w.id.toLowerCase().includes(lowerFilter);
      const labelMatch = w.label ? w.label.toLowerCase().includes(lowerFilter) : false;
      return idMatch || labelMatch;
    });
  }, [response, filterText]);

  return (
    <div className="panel">
      <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: 12 }}>
        <h2 style={{ marginTop: 0, marginBottom: 0 }}>Workflows</h2>
        <div style={{ display: "flex", gap: 8 }}>
          <button className="btn" type="button" onClick={() => setShowCreateModal(true)}>
            + New
          </button>
          <button className="btn" type="button" onClick={() => {
            const isCancelled = { value: false };
            void reload(isCancelled, true);
          }} disabled={loading}>
            Refresh
          </button>
        </div>
      </div>

      <div style={{ marginTop: 12, marginBottom: 8 }}>
        <input
          type="text"
          placeholder="Filter by id or label…"
          value={filterText}
          onChange={(e) => setFilterText(e.target.value)}
          style={{ width: 280, padding: "6px 10px" }}
        />
      </div>

      {loading ? <div className="muted">Loading…</div> : null}
      {statusText ? <div className="small">{statusText}</div> : null}
      {error ? <div className="errorText">Error: {error}</div> : null}

      {!loading && !error && workflows.length === 0
        ? (
          <div className="muted">
            {filterText.trim().length > 0
              ? `No workflows match "${filterText}".`
              : "No workflows found."}
          </div>
        )
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

      {(response?.broken ?? []).length > 0 ? (
        <div style={{ maxWidth: 860, marginTop: 16 }}>
          <h3 style={{ color: "#f87171", marginBottom: 8 }}>Broken Workflows</h3>
          {(response?.broken ?? []).map((b: BrokenWorkflowItem) => (
            <div key={b.id} className="card" style={{ display: "flex", gap: 12, alignItems: "center", justifyContent: "space-between", borderLeft: "3px solid #f87171" }}>
              <div>
                <div style={{ fontWeight: 700, color: "#f87171" }}>{b.id}</div>
                <div className="small">{b.path ?? "path unknown"}</div>
                <div className="small" style={{ color: "#fbbf24", marginTop: 4 }}>{b.error}</div>
              </div>
            </div>
          ))}
        </div>
      ) : null}

      {props.onCreateFromTemplate
        ? (
          <TemplateBrowser
            onCreateFromTemplate={props.onCreateFromTemplate}
          />
        )
        : null}

      <div className="card" style={{ maxWidth: 860 }}>
        <div style={{ fontWeight: 700, marginBottom: 6 }}>Tip</div>
        <div className="small">
          Workflows are loaded from JarvisAgent via <code>/api/workflows</code>.
          The editor will load the selected workflow by id.
        </div>
      </div>

      <CreateWorkflowModal
        isOpen={showCreateModal}
        title="Create New Workflow"
        submitLabel="Create"
        onCancel={() => setShowCreateModal(false)}
        onSubmit={(workflowId) => {
          setShowCreateModal(false);
          props.onOpenWorkflow({ id: workflowId });
        }}
      />
    </div>
  );
}
