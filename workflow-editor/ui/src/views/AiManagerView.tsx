import React, { useCallback, useEffect, useState } from "react";
import {
  listAiInterfaces,
  createAiInterface,
  updateAiInterface,
  deleteAiInterface,
  saveAiInterfaces,
  reloadConfig,
  testAiInterface,
  type AiInterface,
} from "../api/aiInterfaces";
import { listProviders, type ProviderEntry } from "../api/providers";

type EditingInterface = {
  name: string;
  description: string;
  url: string;
  model: string;
  api_type: string;
  key_name: string;
  isNew: boolean;
  originalName: string;
};

function emptyInterface(): EditingInterface
{
  return {
    name: "",
    description: "",
    url: "https://",
    model: "",
    api_type: "API1",
    key_name: "",
    isNew: true,
    originalName: "",
  };
}

function fromEntry(entry: AiInterface): EditingInterface
{
  return {
    name: entry.name,
    description: entry.description,
    url: entry.url,
    model: entry.model,
    api_type: entry.api_type,
    key_name: entry.key_name,
    isNew: false,
    originalName: entry.name,
  };
}

type AiManagerViewProps = {
  onDirtyStateChange?: (dirty: boolean) => void;
};

export default function AiManagerView({ onDirtyStateChange }: AiManagerViewProps): JSX.Element
{
  const [interfaces, setInterfaces] = useState<AiInterface[]>([]);
  const [apiIndex, setApiIndex] = useState<number>(0);
  const [keys, setKeys] = useState<ProviderEntry[]>([]);
  const [editing, setEditing] = useState<EditingInterface | null>(null);
  const [statusMessage, setStatusMessage] = useState<string>("");
  const [errorMessage, setErrorMessage] = useState<string>("");
  const [loading, setLoading] = useState<boolean>(true);
  const [dirty, setDirty] = useState<boolean>(false);
  const [testStatus, setTestStatus] = useState<Record<number, "idle" | "testing" | "success" | "error">>({})
  const [testDetails, setTestDetails] = useState<Record<number, string>>({});

  const refresh = useCallback(async () => {
    try
    {
      setLoading(true);
      const [ifaceResult, keyResult] = await Promise.all([
        listAiInterfaces(),
        listProviders(),
      ]);
      setInterfaces(ifaceResult.interfaces);
      setApiIndex(ifaceResult.api_index);
      setKeys(keyResult.providers);
      setDirty(ifaceResult.dirty);
      onDirtyStateChange?.(ifaceResult.dirty);
    }
    catch (err: unknown)
    {
      setErrorMessage(err instanceof Error ? err.message : "Failed to load AI interfaces");
    }
    finally
    {
      setLoading(false);
    }
  }, [onDirtyStateChange]);

  useEffect(() => { refresh(); }, [refresh]);

  const handleDelete = useCallback(async (name: string) => {
    if (!window.confirm(`Delete AI interface "${name}"?`))
    {
      return;
    }
    const result = await deleteAiInterface(name);
    if (result.ok)
    {
      setStatusMessage(`Deleted "${name}"`);
      setErrorMessage("");
      if (editing && editing.originalName === name)
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

    if (!editing.url.trim())
    {
      setErrorMessage("URL is required");
      return;
    }

    if (editing.isNew)
    {
      const result = await createAiInterface({
        url: editing.url.trim(),
        model: editing.model.trim(),
        api_type: editing.api_type,
        name: editing.name.trim() || undefined,
        description: editing.description.trim() || undefined,
        key_name: editing.key_name || undefined,
      });
      if (result.ok)
      {
        setStatusMessage(`Created "${result.name ?? editing.name}"`);
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
      const result = await updateAiInterface(editing.originalName, {
        url: editing.url.trim(),
        model: editing.model.trim(),
        api_type: editing.api_type,
        name: editing.name.trim(),
        description: editing.description.trim(),
        key_name: editing.key_name,
      });
      if (result.ok)
      {
        setStatusMessage(`Updated "${result.name ?? editing.name}"`);
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

  const handleSaveToConfig = useCallback(async () => {
    setStatusMessage("Saving to config.json...");
    setErrorMessage("");
    const result = await saveAiInterfaces();
    if (result.ok)
    {
      setStatusMessage(`Saved to ${result.path ?? "config.json"}`);
      await refresh();
    }
    else
    {
      setErrorMessage(result.message ?? "Save failed");
      setStatusMessage("");
    }
  }, [refresh]);

  const handleTest = useCallback(async (index: number) => {
    setTestStatus((prev) => ({ ...prev, [index]: "testing" }));
    setTestDetails((prev) => ({ ...prev, [index]: "" }));
    try
    {
      const result = await testAiInterface(index);
      if (result.ok)
      {
        setTestStatus((prev) => ({ ...prev, [index]: "success" }));
        setTestDetails((prev) => ({ ...prev, [index]: `${result.latency_ms}ms` }));
      }
      else
      {
        setTestStatus((prev) => ({ ...prev, [index]: "error" }));
        setTestDetails((prev) => ({ ...prev, [index]: result.error ?? "Test failed" }));
      }
    }
    catch (err: unknown)
    {
      setTestStatus((prev) => ({ ...prev, [index]: "error" }));
      setTestDetails((prev) => ({ ...prev, [index]: err instanceof Error ? err.message : "Network error" }));
    }
  }, []);

  const handleReloadConfig = useCallback(async () => {
    setStatusMessage("Reloading config.json...");
    setErrorMessage("");
    const result = await reloadConfig();
    if (result.ok)
    {
      setStatusMessage(`Reloaded — ${result.interface_count ?? 0} interfaces`);
      await refresh();
    }
    else
    {
      setErrorMessage(result.message ?? "Reload failed");
      setStatusMessage("");
    }
  }, [refresh]);

  const field = (
    label: string,
    key: keyof EditingInterface,
    placeholder: string,
  ) => (
    <div className="field" style={{ marginBottom: 10 }}>
      <label style={{ fontSize: 12, opacity: 0.8 }}>{label}</label>
      <input
        className="input"
        type="text"
        placeholder={placeholder}
        value={editing ? (editing[key] as string) : ""}
        onChange={(e) =>
          setEditing((prev) => prev ? { ...prev, [key]: e.target.value } : prev)
        }
      />
    </div>
  );

  return (
    <div className="panel" style={{ maxWidth: 720, margin: "0 auto" }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 16 }}>
        <h2 style={{ margin: 0 }}>AI Interfaces{dirty ? " *" : ""}</h2>
        <div style={{ display: "flex", gap: 8 }}>
          <button className="btn" type="button" onClick={() => setEditing(emptyInterface())}>
            + Add Interface
          </button>
          <button
            className="btn btnPrimary"
            type="button"
            onClick={handleSaveToConfig}
            disabled={!dirty}
            title={dirty ? "Save changes to config.json" : "No unsaved changes"}
          >
            Save to config.json
          </button>
          <button className="btn" type="button" onClick={handleReloadConfig} title="Reload config.json from disk">
            Reload
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
      ) : interfaces.length === 0 && !editing ? (
        <div className="card">
          <div className="muted" style={{ textAlign: "center", padding: 20 }}>
            No AI interfaces configured. Click "+ Add Interface" to get started.
          </div>
        </div>
      ) : (
        <div>
          {interfaces.map((iface, idx) => (
            <div
              key={iface.name}
              className="card"
              style={{
                display: "flex",
                justifyContent: "space-between",
                alignItems: "center",
                borderColor: idx === apiIndex ? "rgba(120,180,255,0.55)" : undefined,
              }}
            >
              <div>
                <div style={{ fontWeight: 700, fontSize: 14 }}>
                  {iface.name}
                  {idx === apiIndex && (
                    <span style={{ marginLeft: 8, fontSize: 11, opacity: 0.7, fontWeight: 400 }}>
                      (active)
                    </span>
                  )}
                </div>
                {iface.description && (
                  <div className="small muted" style={{ marginTop: 2, fontStyle: "italic" }}>
                    {iface.description}
                  </div>
                )}
                <div className="small muted" style={{ marginTop: 2 }}>
                  {iface.api_type} &middot; {iface.model || "no model"}
                </div>
                <div className="small muted" style={{ marginTop: 2 }}>
                  {iface.url}
                </div>
                <div style={{ marginTop: 4, display: "flex", alignItems: "center", gap: 6 }}>
                  <label className="small muted" style={{ flexShrink: 0 }}>Key:</label>
                  <select
                    className="input"
                    style={{ fontSize: 12, padding: "2px 6px", maxWidth: 260 }}
                    value={iface.key_name || ""}
                    onChange={async (e) => {
                      const newKeyName = e.target.value;
                      await updateAiInterface(iface.name, { key_name: newKeyName });
                      await refresh();
                    }}
                  >
                    {keys.length === 0 ? (
                      <option value="">no key configured</option>
                    ) : (
                      <>
                        <option value="">— not set —</option>
                        {keys.map((k) => (
                          <option key={k.name} value={k.name}>{k.name}{k.has_key ? "" : " (no key)"}</option>
                        ))}
                      </>
                    )}
                  </select>
                </div>
              </div>

              <div style={{ display: "flex", gap: 6, flexShrink: 0, marginLeft: 12, alignItems: "center" }}>
                <span
                  title={testDetails[idx] || "Not tested"}
                  style={{
                    display: "inline-block",
                    width: 10,
                    height: 10,
                    borderRadius: "50%",
                    background:
                      testStatus[idx] === "success" ? "#4cff72"
                      : testStatus[idx] === "error" ? "#ff4c4c"
                      : testStatus[idx] === "testing" ? "#ffcc00"
                      : "#555",
                    boxShadow:
                      testStatus[idx] === "success" ? "0 0 6px #4cff72"
                      : testStatus[idx] === "error" ? "0 0 6px #ff4c4c"
                      : testStatus[idx] === "testing" ? "0 0 6px #ffcc00"
                      : "none",
                    transition: "background 0.3s, box-shadow 0.3s",
                    animation: testStatus[idx] === "testing" ? "pulse 1s infinite" : undefined,
                  }}
                />
                <button
                  className="btn"
                  type="button"
                  onClick={() => handleTest(idx)}
                  disabled={testStatus[idx] === "testing"}
                  title="Send a test prompt to this AI interface"
                  style={{ fontSize: 12, padding: "4px 8px" }}
                >
                  {testStatus[idx] === "testing" ? "Testing…" : "Test"}
                </button>
                <button className="btn" type="button" onClick={() => setEditing(fromEntry(iface))}>
                  Edit
                </button>
                <button className="btn" type="button" onClick={() => handleDelete(iface.name)} style={{ color: "#ff8a8a" }}>
                  Delete
                </button>
              </div>
            </div>
          ))}
        </div>
      )}

      {editing && (
        <div
          style={{
            position: "fixed",
            inset: 0,
            zIndex: 1000,
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            background: "rgba(0,0,0,0.55)",
            backdropFilter: "blur(4px)",
          }}
          onMouseDown={(e) => { if (e.target === e.currentTarget) setEditing(null); }}
        >
          <div
            className="card"
            style={{
              width: "100%",
              maxWidth: 520,
              maxHeight: "85vh",
              overflowY: "auto",
              borderColor: "rgba(120,180,255,0.35)",
              margin: 16,
              boxShadow: "0 8px 32px rgba(0,0,0,0.45)",
            }}
          >
            <h3 style={{ margin: "0 0 12px 0", fontSize: 15 }}>
              {editing.isNew ? "Add AI Interface" : `Edit: ${editing.originalName}`}
            </h3>
            {field("Name (unique key — auto-generated from URL domain + model if empty)", "name", "e.g. api.openai.com/gpt-4o")}
            {field("Description", "description", "e.g. OpenAI GPT-4o")}
            {field("URL", "url", "https://api.openai.com/v1/chat/completions")}
            {field("Model", "model", "e.g. gpt-4o")}
            <div className="field" style={{ marginBottom: 10 }}>
              <label style={{ fontSize: 12, opacity: 0.8 }}>API Type</label>
              <select
                className="input"
                value={editing.api_type}
                onChange={(e) => setEditing((prev) => prev ? { ...prev, api_type: e.target.value } : prev)}
              >
                <option value="API1">API1 (OpenAI-compatible)</option>
                <option value="API2">API2 (Anthropic-style)</option>
                <option value="API3">API3 (Gemini native)</option>
              </select>
            </div>

            <div className="field" style={{ marginBottom: 10 }}>
              <label style={{ fontSize: 12, opacity: 0.8 }}>API Key</label>
              <select
                className="input"
                value={editing.key_name}
                onChange={(e) => setEditing((prev) => prev ? { ...prev, key_name: e.target.value } : prev)}
              >
                {keys.length === 0 ? (
                  <option value="">no key configured</option>
                ) : (
                  <>
                    <option value="">— not set —</option>
                    {keys.map((k) => (
                      <option key={k.name} value={k.name}>{k.name}{k.has_key ? "" : " (no key)"}</option>
                    ))}
                  </>
                )}
              </select>
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
        </div>
      )}
    </div>
  );
}
