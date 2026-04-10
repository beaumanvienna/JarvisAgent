import React, { useCallback, useEffect, useState } from "react";
import {
  listConnections,
  createConnection,
  updateConnection,
  deleteConnection,
  testConnection,
  saveConnections,
  type ConnectionEntry,
} from "../api/connections";

const CONNECTION_TYPES = ["polarion", "s3", "onedrive", "snowflake", "postgres", "slack", "email", "github", "jira"];
const AUTH_TYPES = ["bearer", "oauth2", "jwt_rsa", "basic_auth", "sigv4"];

type EditingConnection = {
  name: string;
  type: string;
  endpoint: string;
  key_name: string;
  auth_type: string;
  params: Record<string, string>;
  isNew: boolean;
};

function emptyConnection(): EditingConnection
{
  return { name: "", type: "polarion", endpoint: "", key_name: "", auth_type: "bearer", params: {}, isNew: true };
}

type ConnectionsViewProps = {
  onDirtyStateChange?: (dirty: boolean) => void;
};

export default function ConnectionsView({ onDirtyStateChange }: ConnectionsViewProps): JSX.Element
{
  const [connections, setConnections] = useState<ConnectionEntry[]>([]);
  const [editing, setEditing] = useState<EditingConnection | null>(null);
  const [statusMessage, setStatusMessage] = useState("");
  const [errorMessage, setErrorMessage] = useState("");
  const [loading, setLoading] = useState(true);
  const [dirty, setDirty] = useState(false);
  const [testingName, setTestingName] = useState<string | null>(null);
  const [paramKey, setParamKey] = useState("");
  const [paramValue, setParamValue] = useState("");

  const refresh = useCallback(async () => {
    try
    {
      setLoading(true);
      const result = await listConnections();
      setConnections(result.connections);
      setDirty(result.dirty);
      onDirtyStateChange?.(result.dirty);
    }
    catch (err: unknown)
    {
      setErrorMessage(err instanceof Error ? err.message : "Failed to load connections");
    }
    finally
    {
      setLoading(false);
    }
  }, [onDirtyStateChange]);

  useEffect(() => { refresh(); }, [refresh]);

  const handleDelete = useCallback(async (name: string) => {
    if (!window.confirm(`Delete connection "${name}"?`)) return;
    const result = await deleteConnection(name);
    if (result.ok)
    {
      setStatusMessage(`Deleted "${name}"`);
      setErrorMessage("");
      if (editing && editing.name === name) setEditing(null);
      await refresh();
    }
    else
    {
      setErrorMessage(result.message ?? "Delete failed");
    }
  }, [editing, refresh]);

  const handleTest = useCallback(async (name: string) => {
    setTestingName(name);
    setStatusMessage("");
    setErrorMessage("");
    const result = await testConnection(name);
    setTestingName(null);
    if (result.ok)
    {
      setStatusMessage(`Connection "${name}" is working`);
    }
    else
    {
      setErrorMessage(result.message ?? "Test failed");
    }
  }, []);

  const handleSave = useCallback(async () => {
    if (!editing) return;
    if (!editing.name.trim())
    {
      setErrorMessage("Name is required");
      return;
    }

    if (editing.isNew)
    {
      const result = await createConnection({
        name: editing.name.trim(),
        type: editing.type,
        endpoint: editing.endpoint,
        key_name: editing.key_name,
        auth_type: editing.auth_type,
        params: Object.keys(editing.params).length > 0 ? editing.params : undefined,
      });
      if (result.ok)
      {
        setStatusMessage(`Created connection "${editing.name.trim()}"`);
        setErrorMessage("");
        setEditing(null);
        await refresh();
      }
      else
      {
        setErrorMessage(result.message ?? "Create failed");
      }
    }
    else
    {
      const result = await updateConnection(editing.name, {
        type: editing.type,
        endpoint: editing.endpoint,
        key_name: editing.key_name,
        auth_type: editing.auth_type,
        params: editing.params,
      });
      if (result.ok)
      {
        setStatusMessage(`Updated connection "${editing.name}"`);
        setErrorMessage("");
        setEditing(null);
        await refresh();
      }
      else
      {
        setErrorMessage(result.message ?? "Update failed");
      }
    }
  }, [editing, refresh]);

  const handlePersist = useCallback(async () => {
    setStatusMessage("Saving connections...");
    setErrorMessage("");
    const result = await saveConnections();
    if (result.ok)
    {
      setStatusMessage(`Saved to ${result.path ?? "connections.json"}`);
      await refresh();
    }
    else
    {
      setErrorMessage(result.message ?? "Save failed");
      setStatusMessage("");
    }
  }, [refresh]);

  const addParam = useCallback(() => {
    if (!paramKey.trim() || !editing) return;
    setEditing({ ...editing, params: { ...editing.params, [paramKey.trim()]: paramValue } });
    setParamKey("");
    setParamValue("");
  }, [paramKey, paramValue, editing]);

  const removeParam = useCallback((key: string) => {
    if (!editing) return;
    const newParams = { ...editing.params };
    delete newParams[key];
    setEditing({ ...editing, params: newParams });
  }, [editing]);

  return (
    <div className="panel" style={{ maxWidth: 720, margin: "0 auto" }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 16 }}>
        <h2 style={{ margin: 0 }}>Connections{dirty ? " *" : ""}</h2>
        <div style={{ display: "flex", gap: 8 }}>
          <button className="btn" type="button" onClick={() => setEditing(emptyConnection())}>
            + Add
          </button>
          <button className="btn btnPrimary" type="button" onClick={handlePersist}>
            Save
          </button>
        </div>
      </div>

      {statusMessage && (
        <div style={{ marginBottom: 10, padding: "8px 12px", borderRadius: 8, background: "rgba(120,255,170,0.08)", border: "1px solid rgba(120,255,170,0.25)", fontSize: 13 }}>
          {statusMessage}
        </div>
      )}
      {errorMessage && (
        <div style={{ marginBottom: 10, padding: "8px 12px", borderRadius: 8, background: "rgba(255,120,120,0.08)", border: "1px solid rgba(255,120,120,0.25)", fontSize: 13, color: "#ff8a8a" }}>
          {errorMessage}
        </div>
      )}

      {loading ? (
        <div className="muted">Loading...</div>
      ) : connections.length === 0 && !editing ? (
        <div className="card">
          <div className="muted" style={{ textAlign: "center", padding: 20 }}>
            No cloud connections configured. Click "+ Add" to get started.
          </div>
        </div>
      ) : (
        <div>
          {connections.map((c) => (
            <div key={c.name} className="card" style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
              <div>
                <div style={{ fontWeight: 700, fontSize: 14 }}>
                  {c.name}
                </div>
                <div className="small muted" style={{ marginTop: 2 }}>
                  {c.type}{" \u2022 "}{c.endpoint || "(no endpoint)"}{c.key_name ? ` \u2022 key: ${c.key_name}` : ""}
                </div>
              </div>
              <div style={{ display: "flex", gap: 6, flexShrink: 0, marginLeft: 12 }}>
                <button
                  className="btn"
                  type="button"
                  onClick={() => handleTest(c.name)}
                  disabled={testingName === c.name}
                >
                  {testingName === c.name ? "Testing..." : "Test"}
                </button>
                <button
                  className="btn"
                  type="button"
                  onClick={() => setEditing({
                    name: c.name,
                    type: c.type,
                    endpoint: c.endpoint,
                    key_name: c.key_name,
                    auth_type: c.auth_type,
                    params: { ...c.params },
                    isNew: false,
                  })}
                >
                  Edit
                </button>
                <button className="btn" type="button" onClick={() => handleDelete(c.name)} style={{ color: "#ff8a8a" }}>
                  Delete
                </button>
              </div>
            </div>
          ))}
        </div>
      )}

      {editing && (
        <div className="card" style={{ marginTop: 16, borderColor: "rgba(120,180,255,0.35)" }}>
          <h3 style={{ margin: "0 0 12px 0", fontSize: 15 }}>
            {editing.isNew ? "Add Connection" : `Edit: ${editing.name}`}
          </h3>

          {editing.isNew && (
            <div className="field" style={{ marginBottom: 10 }}>
              <label style={{ fontSize: 12, opacity: 0.8 }}>Name</label>
              <input
                className="input"
                type="text"
                placeholder="e.g. my-polarion, production-s3"
                value={editing.name}
                onChange={(e) => setEditing((prev) => prev ? { ...prev, name: e.target.value } : prev)}
              />
            </div>
          )}

          <div className="field" style={{ marginBottom: 10 }}>
            <label style={{ fontSize: 12, opacity: 0.8 }}>Type</label>
            <select
              className="input"
              value={editing.type}
              onChange={(e) => setEditing((prev) => prev ? { ...prev, type: e.target.value } : prev)}
            >
              {CONNECTION_TYPES.map((t) => <option key={t} value={t}>{t}</option>)}
            </select>
          </div>

          <div className="field" style={{ marginBottom: 10 }}>
            <label style={{ fontSize: 12, opacity: 0.8 }}>Endpoint</label>
            <input
              className="input"
              type="text"
              placeholder="e.g. https://polarion.company.com"
              value={editing.endpoint}
              onChange={(e) => setEditing((prev) => prev ? { ...prev, endpoint: e.target.value } : prev)}
            />
          </div>

          <div className="field" style={{ marginBottom: 10 }}>
            <label style={{ fontSize: 12, opacity: 0.8 }}>Key (from Keys page)</label>
            <input
              className="input"
              type="text"
              placeholder="Key name"
              value={editing.key_name}
              onChange={(e) => setEditing((prev) => prev ? { ...prev, key_name: e.target.value } : prev)}
            />
          </div>

          <div className="field" style={{ marginBottom: 10 }}>
            <label style={{ fontSize: 12, opacity: 0.8 }}>Auth Type</label>
            <select
              className="input"
              value={editing.auth_type}
              onChange={(e) => setEditing((prev) => prev ? { ...prev, auth_type: e.target.value } : prev)}
            >
              {AUTH_TYPES.map((t) => <option key={t} value={t}>{t}</option>)}
            </select>
          </div>

          {editing.type === "s3" && (
            <>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Region</label>
                <input
                  className="input"
                  type="text"
                  placeholder="e.g. us-east-1"
                  value={editing.params.region ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, region: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Bucket</label>
                <input
                  className="input"
                  type="text"
                  placeholder="e.g. my-bucket"
                  value={editing.params.bucket ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, bucket: e.target.value } } : prev)}
                />
              </div>
            </>
          )}

          {editing.type === "polarion" && (
            <div className="field" style={{ marginBottom: 10 }}>
              <label style={{ fontSize: 12, opacity: 0.8 }}>Project ID</label>
              <input
                className="input"
                type="text"
                placeholder="e.g. GoKartProcurement"
                value={editing.params.project_id ?? ""}
                onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, project_id: e.target.value } } : prev)}
              />
            </div>
          )}

          {editing.type === "postgres" && (
            <>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Database</label>
                <input
                  className="input"
                  type="text"
                  placeholder="e.g. mydb"
                  value={editing.params.database ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, database: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>SSL Mode</label>
                <select
                  className="input"
                  value={editing.params.sslmode ?? "prefer"}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, sslmode: e.target.value } } : prev)}
                >
                  <option value="disable">disable</option>
                  <option value="prefer">prefer</option>
                  <option value="require">require</option>
                  <option value="verify-ca">verify-ca</option>
                  <option value="verify-full">verify-full</option>
                </select>
              </div>
            </>
          )}

          <div className="field" style={{ marginBottom: 10 }}>
            <label style={{ fontSize: 12, opacity: 0.8 }}>Parameters</label>
            {Object.entries(editing.params).map(([k, v]) => (
              <div key={k} style={{ display: "flex", gap: 6, marginBottom: 4, alignItems: "center" }}>
                <span className="small" style={{ minWidth: 100 }}>{k}</span>
                <span className="small muted" style={{ flex: 1 }}>{v}</span>
                <button className="btn" type="button" onClick={() => removeParam(k)} style={{ padding: "2px 8px", fontSize: 12, color: "#ff8a8a" }}>x</button>
              </div>
            ))}
            <div style={{ display: "flex", gap: 6, marginTop: 4 }}>
              <input
                className="input"
                type="text"
                placeholder="Key"
                value={paramKey}
                onChange={(e) => setParamKey(e.target.value)}
                style={{ flex: 1 }}
              />
              <input
                className="input"
                type="text"
                placeholder="Value"
                value={paramValue}
                onChange={(e) => setParamValue(e.target.value)}
                style={{ flex: 2 }}
              />
              <button className="btn" type="button" onClick={addParam} style={{ flexShrink: 0 }}>Add</button>
            </div>
          </div>

          <div style={{ display: "flex", gap: 8, marginTop: 12 }}>
            <button className="btn btnPrimary" type="button" onClick={handleSave}>
              {editing.isNew ? "Create" : "Update"}
            </button>
            <button className="btn" type="button" onClick={() => setEditing(null)}>
              Cancel
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
