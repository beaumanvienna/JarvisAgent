import React, { useCallback, useEffect, useState } from "react";
import {
  listConnections,
  createConnection,
  updateConnection,
  deleteConnection,
  testConnection,
  saveConnections,
  authorizeConnection,
  type ConnectionEntry,
} from "../api/connections";
import { listProviders, saveProviders } from "../api/providers";

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
  appMasterPassword: string | null;
  onDirtyStateChange?: (dirty: boolean) => void;
};

export default function ConnectionsView({ appMasterPassword, onDirtyStateChange }: ConnectionsViewProps): JSX.Element
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
  const [keyNames, setKeyNames] = useState<string[]>([]);
  const [localMasterPassword, setLocalMasterPassword] = useState<string | null>(null);
  const [showMasterPasswordPrompt, setShowMasterPasswordPrompt] = useState(false);
  const [masterPasswordInput, setMasterPasswordInput] = useState("");
  const [savePasswordError, setSavePasswordError] = useState<string | null>(null);
  const [showMasterPwVisible, setShowMasterPwVisible] = useState(false);

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

  useEffect(() =>
  {
    listProviders()
      .then((r) => setKeyNames(r.providers.map((p) => p.name)))
      .catch(() => {});
  }, []);

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

  const doSaveAll = useCallback(async (pw: string, fromPrompt: boolean) => {
    setStatusMessage("Saving connections and keys...");
    setErrorMessage("");
    setSavePasswordError(null);

    // Save connections.json
    const connResult = await saveConnections();
    if (!connResult.ok)
    {
      setErrorMessage(connResult.message ?? "Connection save failed");
      setStatusMessage("");
      return;
    }

    // Save encrypted keys file
    const keysResult = await saveProviders(pw);
    if (keysResult.ok)
    {
      setLocalMasterPassword(pw);
      setShowMasterPasswordPrompt(false);
      setStatusMessage("Saved connections and keys.");
      await refresh();
    }
    else if (keysResult.error === "wrong_password")
    {
      setStatusMessage("");
      setLocalMasterPassword(null);
      if (fromPrompt)
      {
        setSavePasswordError(keysResult.message ?? "Incorrect password.");
      }
      else
      {
        setSavePasswordError(null);
        setMasterPasswordInput("");
        setShowMasterPasswordPrompt(true);
        setErrorMessage(keysResult.message ?? "Incorrect password.");
      }
    }
    else
    {
      setErrorMessage(keysResult.message ?? "Keys save failed");
      setStatusMessage("");
    }
  }, [refresh]);

  const handlePersist = useCallback(async () => {
    const pw = localMasterPassword ?? appMasterPassword;
    if (pw)
    {
      await doSaveAll(pw, false);
    }
    else
    {
      setMasterPasswordInput("");
      setSavePasswordError(null);
      setShowMasterPasswordPrompt(true);
    }
  }, [localMasterPassword, appMasterPassword, doSaveAll]);

  const handleMasterPasswordSubmit = useCallback(async () => {
    if (!masterPasswordInput.trim())
    {
      setSavePasswordError("Master password cannot be empty.");
      return;
    }
    await doSaveAll(masterPasswordInput, true);
  }, [masterPasswordInput, doSaveAll]);

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
        <h2 style={{ margin: 0 }}>Connections</h2>
        <div style={{ display: "flex", gap: 8 }}>
          <button className="btn" type="button" onClick={() => setEditing(emptyConnection())}>
            + Add
          </button>
          <button className="btn btnPrimary" type="button" onClick={handlePersist} style={dirty ? { boxShadow: "0 0 0 2px rgba(255,180,60,0.7)" } : {}}>
            {dirty ? "Save *" : "Save"}
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

      {showMasterPasswordPrompt && (
        <div className="modalOverlay">
          <div className="modalContent" style={{ maxWidth: 420 }}>
            <div className="modalHeader">
              <h2 style={{ margin: 0, fontSize: 16 }}>Master Password Required</h2>
            </div>
            <form onSubmit={(e) => { e.preventDefault(); handleMasterPasswordSubmit(); }}>
              <div className="modalBody">
                <p style={{ marginTop: 0, marginBottom: 16, color: "#ccc" }}>
                  Enter your master password to save connections and encrypt the keys file.
                </p>
                <div style={{ position: "relative" }}>
                  <input
                    type={showMasterPwVisible ? "text" : "password"}
                    value={masterPasswordInput}
                    onChange={(e) => setMasterPasswordInput(e.target.value)}
                    placeholder="Master password"
                    autoFocus
                    style={{
                      width: "100%",
                      padding: "8px 36px 8px 12px",
                      fontSize: 14,
                      borderRadius: 4,
                      border: "1px solid #555",
                      background: "#1e1e1e",
                      color: "#eee",
                      boxSizing: "border-box",
                    }}
                  />
                  <button
                    type="button"
                    onClick={() => setShowMasterPwVisible((prev) => !prev)}
                    style={{
                      position: "absolute",
                      right: 8,
                      top: "50%",
                      transform: "translateY(-50%)",
                      background: "none",
                      border: "none",
                      cursor: "pointer",
                      color: "#aaa",
                      fontSize: 16,
                      padding: 0,
                      lineHeight: 1,
                    }}
                    title={showMasterPwVisible ? "Hide password" : "Show password"}
                  >
                    {showMasterPwVisible ? "\u{1F441}" : "\u{1F441}\u{200D}\u{1F5E8}"}
                  </button>
                </div>
                {savePasswordError && (
                  <div style={{ marginTop: 8, padding: "6px 10px", borderRadius: 4, background: "#3a1c1c", color: "#f88", fontSize: 13 }}>
                    {savePasswordError}
                  </div>
                )}
              </div>
              <div className="modalFooter">
                <button className="btn" type="submit">Save</button>
                <button className="btn" type="button" onClick={() => setShowMasterPasswordPrompt(false)} style={{ marginLeft: 8 }}>Cancel</button>
              </div>
            </form>
          </div>
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
            <select
              className="input"
              value={editing.key_name}
              onChange={(e) => setEditing((prev) => prev ? { ...prev, key_name: e.target.value } : prev)}
            >
              <option value="">— select key —</option>
              {keyNames.map((k) => <option key={k} value={k}>{k}</option>)}
            </select>
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

          {editing.type === "onedrive" && (
            <>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Client ID (Azure AD application ID)</label>
                <input
                  className="input"
                  type="text"
                  placeholder="e.g. 12345678-abcd-..."
                  value={editing.params.client_id ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, client_id: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Tenant ID (default: common)</label>
                <input
                  className="input"
                  type="text"
                  placeholder="common"
                  value={editing.params.tenant_id ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, tenant_id: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Scopes (default: Files.ReadWrite offline_access)</label>
                <input
                  className="input"
                  type="text"
                  placeholder="Files.ReadWrite offline_access"
                  value={editing.params.scopes ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, scopes: e.target.value } } : prev)}
                />
              </div>
              {!editing.isNew && (
                <div style={{ marginBottom: 10 }}>
                  <button
                    className="btn"
                    type="button"
                    style={{ background: "rgba(56,139,253,0.15)", borderColor: "rgba(56,139,253,0.4)" }}
                    onClick={async () => {
                      setStatusMessage("Starting OAuth flow...");
                      setErrorMessage("");
                      try
                      {
                        const result = await authorizeConnection(editing.name);
                        if (result.ok && result.authorize_url)
                        {
                          window.open(result.authorize_url, "_blank", "width=600,height=700");
                          setStatusMessage("Authorization window opened. Complete the login, then test the connection.");
                        }
                        else
                        {
                          setErrorMessage(result.message ?? "Failed to start OAuth flow");
                          setStatusMessage("");
                        }
                      }
                      catch (err: unknown)
                      {
                        setErrorMessage(err instanceof Error ? err.message : "OAuth request failed");
                        setStatusMessage("");
                      }
                    }}
                  >
                    Authorize with Microsoft
                  </button>
                  <div className="small muted" style={{ marginTop: 4 }}>
                    Opens Microsoft login in a new window. After authorizing, tokens are stored automatically.
                  </div>
                </div>
              )}
            </>
          )}

          {editing.type === "snowflake" && (
            <>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Account (e.g. xy12345)</label>
                <input
                  className="input"
                  type="text"
                  placeholder="xy12345"
                  value={editing.params.account ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, account: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>User</label>
                <input
                  className="input"
                  type="text"
                  placeholder="SVC_JARVIS"
                  value={editing.params.user ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, user: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Warehouse</label>
                <input
                  className="input"
                  type="text"
                  placeholder="COMPUTE_WH"
                  value={editing.params.warehouse ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, warehouse: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Database</label>
                <input
                  className="input"
                  type="text"
                  placeholder="ANALYTICS"
                  value={editing.params.database ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, database: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Schema</label>
                <input
                  className="input"
                  type="text"
                  placeholder="PUBLIC"
                  value={editing.params.schema ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, schema: e.target.value } } : prev)}
                />
              </div>
            </>
          )}

          {editing.type === "email" && (
            <>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>SMTP Host</label>
                <input
                  className="input"
                  type="text"
                  placeholder="smtp.gmail.com"
                  value={editing.params.smtp_host ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, smtp_host: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>SMTP Port (587=STARTTLS, 465=SSL)</label>
                <input
                  className="input"
                  type="text"
                  placeholder="587"
                  value={editing.params.smtp_port ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, smtp_port: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>IMAP Host (for email_watch trigger)</label>
                <input
                  className="input"
                  type="text"
                  placeholder="imap.gmail.com"
                  value={editing.params.imap_host ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, imap_host: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>From address (default: credential username)</label>
                <input
                  className="input"
                  type="text"
                  placeholder="alerts@company.com"
                  value={editing.params.from ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, from: e.target.value } } : prev)}
                />
              </div>
            </>
          )}

          {editing.type === "github" && (
            <>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Owner (organization or user)</label>
                <input
                  className="input"
                  type="text"
                  placeholder="myorg"
                  value={editing.params.owner ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, owner: e.target.value } } : prev)}
                />
              </div>
              <div className="field" style={{ marginBottom: 10 }}>
                <label style={{ fontSize: 12, opacity: 0.8 }}>Repository</label>
                <input
                  className="input"
                  type="text"
                  placeholder="myrepo"
                  value={editing.params.repo ?? ""}
                  onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, repo: e.target.value } } : prev)}
                />
              </div>
            </>
          )}

          {editing.type === "jira" && (
            <div className="field" style={{ marginBottom: 10 }}>
              <label style={{ fontSize: 12, opacity: 0.8 }}>Project Key</label>
              <input
                className="input"
                type="text"
                placeholder="PROJ"
                value={editing.params.project_key ?? ""}
                onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, project_key: e.target.value } } : prev)}
              />
            </div>
          )}

          {editing.type === "google_sheets" && (
            <div className="field" style={{ marginBottom: 10 }}>
              <label style={{ fontSize: 12, opacity: 0.8 }}>Spreadsheet ID</label>
              <input
                className="input"
                type="text"
                placeholder="1BxiMVs0XRA5nFMdKvBdBZjgmUUqptlbs74OgVE2upms"
                value={editing.params.spreadsheet_id ?? ""}
                onChange={(e) => setEditing((prev) => prev ? { ...prev, params: { ...prev.params, spreadsheet_id: e.target.value } } : prev)}
              />
            </div>
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
