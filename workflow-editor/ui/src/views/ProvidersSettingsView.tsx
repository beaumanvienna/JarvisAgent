import React, { useCallback, useEffect, useState } from "react";
import {
  listProviders,
  createProvider,
  updateProvider,
  deleteProvider,
  saveProviders,
  type ProviderEntry,
} from "../api/providers";

type EditingKey = {
  name: string;
  api_key: string;
  isNew: boolean;
};

function emptyKey(): EditingKey
{
  return { name: "", api_key: "", isNew: true };
}

type ProvidersSettingsViewProps = {
  appMasterPassword: string | null;
};

export default function ProvidersSettingsView({ appMasterPassword }: ProvidersSettingsViewProps): JSX.Element
{
  const [providers, setProviders] = useState<ProviderEntry[]>([]);
  const [editing, setEditing] = useState<EditingKey | null>(null);
  const [statusMessage, setStatusMessage] = useState<string>("");
  const [errorMessage, setErrorMessage] = useState<string>("");
  const [loading, setLoading] = useState<boolean>(true);
  const [showPassword, setShowPassword] = useState(false);
  const [localMasterPassword, setLocalMasterPassword] = useState<string | null>(null);
  const [showMasterPasswordPrompt, setShowMasterPasswordPrompt] = useState(false);
  const [masterPasswordInput, setMasterPasswordInput] = useState("");
  const [showMasterPwVisible, setShowMasterPwVisible] = useState(false);

  const refresh = useCallback(async () => {
    try
    {
      setLoading(true);
      const provResult = await listProviders();
      setProviders(provResult.providers);
    }
    catch (err: unknown)
    {
      setErrorMessage(err instanceof Error ? err.message : "Failed to load data");
    }
    finally
    {
      setLoading(false);
    }
  }, []);

  useEffect(() => { refresh(); }, [refresh]);

  const handleDelete = useCallback(async (name: string) => {
    if (!window.confirm(`Delete key "${name}"?`))
    {
      return;
    }
    const result = await deleteProvider(name);
    if (result.ok)
    {
      setStatusMessage(`Deleted "${name}"`);
      setErrorMessage("");
      if (editing && editing.name === name)
      {
        setEditing(null);
      }
      await refresh();
    }
    else
    {
      setErrorMessage(result.message ?? "Delete failed");
    }
  }, [editing, refresh]);

  const handleSave = useCallback(async () => {
    if (!editing)
    {
      return;
    }

    if (!editing.name.trim())
    {
      setErrorMessage("Name is required");
      return;
    }

    if (!editing.api_key.trim() && editing.isNew)
    {
      setErrorMessage("API key is required");
      return;
    }

    if (editing.isNew)
    {
      const result = await createProvider({
        name: editing.name.trim(),
        api_key: editing.api_key,
      });
      if (result.ok)
      {
        setStatusMessage(`Created key "${editing.name.trim()}"`);
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
      const updates: Record<string, string> = {};
      if (editing.api_key) updates.api_key = editing.api_key;

      const result = await updateProvider(editing.name, updates);
      if (result.ok)
      {
        setStatusMessage(`Updated key "${editing.name}"`);
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

  const [savePasswordError, setSavePasswordError] = useState<string | null>(null);

  const doSaveWithPassword = useCallback(async (pw: string, fromPrompt: boolean) => {
    setStatusMessage("Saving encrypted keys file...");
    setErrorMessage("");
    setSavePasswordError(null);
    const result = await saveProviders(pw);
    if (result.ok)
    {
      setLocalMasterPassword(pw);
      setShowMasterPasswordPrompt(false);
      setStatusMessage(`Saved to ${result.path ?? "keys.json.enc"}`);
    }
    else if (result.error === "wrong_password")
    {
      setStatusMessage("");
      setLocalMasterPassword(null);
      if (fromPrompt)
      {
        setSavePasswordError(result.message ?? "Incorrect password.");
      }
      else
      {
        setSavePasswordError(null);
        setMasterPasswordInput("");
        setShowMasterPasswordPrompt(true);
        setErrorMessage(result.message ?? "Incorrect password.");
      }
    }
    else
    {
      setErrorMessage(result.message ?? "Save failed");
      setStatusMessage("");
    }
  }, []);

  const handlePersistEncrypted = useCallback(async () => {
    const pw = localMasterPassword ?? appMasterPassword;
    if (pw)
    {
      await doSaveWithPassword(pw, false);
    }
    else
    {
      setMasterPasswordInput("");
      setSavePasswordError(null);
      setShowMasterPasswordPrompt(true);
    }
  }, [localMasterPassword, appMasterPassword, doSaveWithPassword]);

  const handleMasterPasswordSubmit = useCallback(async () => {
    if (!masterPasswordInput.trim())
    {
      setSavePasswordError("Master password cannot be empty.");
      return;
    }
    await doSaveWithPassword(masterPasswordInput, true);
  }, [masterPasswordInput, doSaveWithPassword]);

  return (
    <div className="panel" style={{ maxWidth: 720, margin: "0 auto" }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 16 }}>
        <h2 style={{ margin: 0 }}>AI Keys</h2>
        <div style={{ display: "flex", gap: 8 }}>
          <button className="btn" type="button" onClick={() => setEditing(emptyKey())}>
            + Add Key
          </button>
          <button className="btn btnPrimary" type="button" onClick={handlePersistEncrypted}>
            Save Encrypted
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
      ) : providers.length === 0 && !editing ? (
        <div className="card">
          <div className="muted" style={{ textAlign: "center", padding: 20 }}>
            No API keys configured. Click "+ Add Key" to get started.
          </div>
        </div>
      ) : (
        <div>
          {providers.map((p) => (
            <div
              key={p.name}
              className="card"
              style={{
                display: "flex",
                justifyContent: "space-between",
                alignItems: "center",
              }}
            >
              <div>
                <div style={{ fontWeight: 700, fontSize: 14 }}>
                  {p.name}
                </div>
                <div className="small muted" style={{ marginTop: 2 }}>
                  {p.has_key ? "key set" : "no key"}
                </div>
              </div>

              <div style={{ display: "flex", gap: 6, flexShrink: 0, marginLeft: 12 }}>
                <button
                  className="btn"
                  type="button"
                  onClick={() => setEditing({ name: p.name, api_key: "", isNew: false })}
                >
                  Edit
                </button>
                <button className="btn" type="button" onClick={() => handleDelete(p.name)} style={{ color: "#ff8a8a" }}>
                  Delete
                </button>
              </div>
            </div>
          ))}
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
                  Enter your master password to encrypt and save the keys file.
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
                  <div
                    style={{
                      marginTop: 8,
                      padding: "6px 10px",
                      borderRadius: 4,
                      background: "#3a1c1c",
                      color: "#f88",
                      fontSize: 13,
                    }}
                  >
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

      {editing && (
        <div className="card" style={{ marginTop: 16, borderColor: "rgba(120,180,255,0.35)" }}>
          <h3 style={{ margin: "0 0 12px 0", fontSize: 15 }}>
            {editing.isNew ? "Add Key" : `Edit Key: ${editing.name}`}
          </h3>

          {editing.isNew ? (
            <div className="field" style={{ marginBottom: 10 }}>
              <label style={{ fontSize: 12, opacity: 0.8 }}>Name</label>
              <input
                className="input"
                type="text"
                placeholder="e.g. openai, google, anthropic"
                value={editing.name}
                onChange={(e) => setEditing((prev) => prev ? { ...prev, name: e.target.value } : prev)}
              />
            </div>
          ) : (
            <div className="field" style={{ marginBottom: 10 }}>
              <label style={{ fontSize: 12, opacity: 0.8 }}>Name</label>
              <input className="input" type="text" value={editing.name} disabled />
            </div>
          )}

          <div className="field" style={{ marginBottom: 10 }}>
            <label style={{ fontSize: 12, opacity: 0.8 }}>API Key</label>
            <div style={{ position: "relative" }}>
              <input
                className="input"
                type={showPassword ? "text" : "password"}
                placeholder={editing.isNew ? "sk-..." : "(leave blank to keep current)"}
                value={editing.api_key}
                onChange={(e) => setEditing((prev) => prev ? { ...prev, api_key: e.target.value } : prev)}
                style={{ paddingRight: 36 }}
              />
              <button
                type="button"
                onClick={() => setShowPassword((prev) => !prev)}
                title={showPassword ? "Hide key" : "Show key"}
                style={{
                  position: "absolute",
                  right: 6,
                  top: "50%",
                  transform: "translateY(-50%)",
                  background: "none",
                  border: "none",
                  cursor: "pointer",
                  fontSize: 16,
                  padding: "2px 4px",
                  opacity: 0.7,
                }}
              >
                {showPassword ? "\u{1F441}" : "\u{1F441}\u200D\u{1F5E8}"}
              </button>
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
