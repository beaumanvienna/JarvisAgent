import { useCallback, useEffect, useState } from "react";
import {
  listAiInterfaces,
  createAiInterface,
  updateAiInterface,
  deleteAiInterface,
  saveAiInterfaces,
  reloadConfig,
  testAiInterface,
  type AiInterface,
  type RateLimitConfig,
} from "../api/aiInterfaces";
import { listProviders, saveProviders, type ProviderEntry } from "../api/providers";

type EditingInterface = {
  name: string;
  description: string;
  url: string;
  model: string;
  api_type: string;
  key_name: string;
  // Rate-limit knobs — empty string means "use server default"; numeric values
  // override.  Stored as strings here so the form can distinguish "not set" from
  // "explicit zero" cleanly; converted to numbers (or omitted entirely) at submit.
  rl_initial_concurrency_probe: string;
  rl_max_concurrency: string;
  rl_max_retries_429: string;
  rl_max_retries_transient: string;
  rl_base_retry_ms: string;
  rb_per_1k_input_token_seconds: string;
  rb_per_1k_output_token_seconds: string;
  rb_fixed_overhead_seconds: string;
  rb_safety_margin_factor: string;
  rb_min_seconds: string;
  rb_max_seconds: string;
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
    rl_initial_concurrency_probe: "",
    rl_max_concurrency: "",
    rl_max_retries_429: "",
    rl_max_retries_transient: "",
    rl_base_retry_ms: "",
    rb_per_1k_input_token_seconds: "",
    rb_per_1k_output_token_seconds: "",
    rb_fixed_overhead_seconds: "",
    rb_safety_margin_factor: "",
    rb_min_seconds: "",
    rb_max_seconds: "",
    isNew: true,
    originalName: "",
  };
}

function fromEntry(entry: AiInterface): EditingInterface
{
  const rl = entry.rate_limit ?? {};
  const rb = rl.request_budget ?? {};
  const numStr = (v: number | undefined): string => (v === undefined || v === null ? "" : String(v));
  return {
    name: entry.name,
    description: entry.description,
    url: entry.url,
    model: entry.model,
    api_type: entry.api_type,
    key_name: entry.key_name,
    rl_initial_concurrency_probe: numStr(rl.initial_concurrency_probe),
    rl_max_concurrency: numStr(rl.max_concurrency),
    rl_max_retries_429: numStr(rl.max_retries_429),
    rl_max_retries_transient: numStr(rl.max_retries_transient),
    rl_base_retry_ms: numStr(rl.base_retry_ms),
    rb_per_1k_input_token_seconds: numStr(rb.per_1k_input_token_seconds),
    rb_per_1k_output_token_seconds: numStr(rb.per_1k_output_token_seconds),
    rb_fixed_overhead_seconds: numStr(rb.fixed_overhead_seconds),
    rb_safety_margin_factor: numStr(rb.safety_margin_factor),
    rb_min_seconds: numStr(rb.min_seconds),
    rb_max_seconds: numStr(rb.max_seconds),
    isNew: false,
    originalName: entry.name,
  };
}

// Build a RateLimitConfig from an EditingInterface.  Returns undefined when
// every knob is left empty (server keeps its baked-in defaults).  Empty
// individual fields are dropped from the payload so partial overrides work.
function buildRateLimitPayload(e: EditingInterface): RateLimitConfig | undefined
{
  const parseInt_ = (s: string): number | undefined =>
  {
    const t = s.trim();
    if (t === "") return undefined;
    const n = parseInt(t, 10);
    return Number.isFinite(n) ? n : undefined;
  };
  const parseFloat_ = (s: string): number | undefined =>
  {
    const t = s.trim();
    if (t === "") return undefined;
    const n = parseFloat(t);
    return Number.isFinite(n) ? n : undefined;
  };

  const budget = {
    per_1k_input_token_seconds: parseFloat_(e.rb_per_1k_input_token_seconds),
    per_1k_output_token_seconds: parseFloat_(e.rb_per_1k_output_token_seconds),
    fixed_overhead_seconds: parseFloat_(e.rb_fixed_overhead_seconds),
    safety_margin_factor: parseFloat_(e.rb_safety_margin_factor),
    min_seconds: parseFloat_(e.rb_min_seconds),
    max_seconds: parseFloat_(e.rb_max_seconds),
  };
  const budgetSet = Object.values(budget).some((v) => v !== undefined);

  const rl: RateLimitConfig = {
    initial_concurrency_probe: parseInt_(e.rl_initial_concurrency_probe),
    max_concurrency: parseInt_(e.rl_max_concurrency),
    max_retries_429: parseInt_(e.rl_max_retries_429),
    max_retries_transient: parseInt_(e.rl_max_retries_transient),
    base_retry_ms: parseInt_(e.rl_base_retry_ms),
  };
  if (budgetSet)
  {
    // Only include defined entries — partial overrides leave other budget
    // fields at their server defaults.
    const cleaned: Record<string, number> = {};
    for (const [k, v] of Object.entries(budget))
    {
      if (v !== undefined) cleaned[k] = v;
    }
    rl.request_budget = cleaned;
  }

  // Strip undefined top-level keys so the payload is clean.
  const cleaned: RateLimitConfig = {};
  for (const [k, v] of Object.entries(rl))
  {
    if (v !== undefined) (cleaned as Record<string, unknown>)[k] = v;
  }
  return Object.keys(cleaned).length > 0 ? cleaned : undefined;
}

type AiManagerViewProps = {
  appMasterPassword: string | null;
  onDirtyStateChange?: (dirty: boolean) => void;
};

export default function AiManagerView({ appMasterPassword, onDirtyStateChange }: AiManagerViewProps): JSX.Element
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
  const [localMasterPassword, setLocalMasterPassword] = useState<string | null>(null);
  const [showMasterPasswordPrompt, setShowMasterPasswordPrompt] = useState(false);
  const [masterPasswordInput, setMasterPasswordInput] = useState("");
  const [savePasswordError, setSavePasswordError] = useState<string | null>(null);
  const [showMasterPwVisible, setShowMasterPwVisible] = useState(false);

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

    const ratePayload = buildRateLimitPayload(editing);

    if (editing.isNew)
    {
      const result = await createAiInterface({
        url: editing.url.trim(),
        model: editing.model.trim(),
        api_type: editing.api_type,
        name: editing.name.trim() || undefined,
        description: editing.description.trim() || undefined,
        key_name: editing.key_name || undefined,
        rate_limit: ratePayload,
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
        rate_limit: ratePayload,
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

  const doSaveAll = useCallback(async (pw: string, fromPrompt: boolean) => {
    setStatusMessage("Saving config and keys...");
    setErrorMessage("");
    setSavePasswordError(null);

    const configResult = await saveAiInterfaces();
    if (!configResult.ok)
    {
      setErrorMessage(configResult.message ?? "Config save failed");
      setStatusMessage("");
      return;
    }

    const keysResult = await saveProviders(pw);
    if (keysResult.ok)
    {
      setLocalMasterPassword(pw);
      setShowMasterPasswordPrompt(false);
      setStatusMessage("Saved config and keys.");
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

  const handleSaveToConfig = useCallback(async () => {
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

  const handleTest = useCallback(async (index: number) => {
    setTestStatus((prev) => ({ ...prev, [index]: "testing" }));
    setTestDetails((prev) => ({ ...prev, [index]: "" }));
    // Minimum visible yellow time. Without this, fast failures (ECONNREFUSED in 0ms)
    // collapse "testing" and "error" into a single render and the user sees red instantly.
    const minVisibleMs = 250;
    const startedAt = performance.now();
    const settle = (status: "success" | "error", detail: string) => {
      const elapsed = performance.now() - startedAt;
      const wait = Math.max(0, minVisibleMs - elapsed);
      setTimeout(() => {
        setTestStatus((prev) => ({ ...prev, [index]: status }));
        setTestDetails((prev) => ({ ...prev, [index]: detail }));
      }, wait);
    };
    try
    {
      const result = await testAiInterface(index);
      if (result.ok)
      {
        settle("success", `${result.latency_ms}ms`);
      }
      else
      {
        settle("error", result.error ?? "Test failed");
      }
    }
    catch (err: unknown)
    {
      settle("error", err instanceof Error ? err.message : "Network error");
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
        <h2 style={{ margin: 0 }}>AI Interfaces</h2>
        <div style={{ display: "flex", gap: 8 }}>
          <button className="btn" type="button" onClick={() => setEditing(emptyInterface())}>
            + Add Interface
          </button>
          <button
            className="btn btnPrimary"
            type="button"
            onClick={handleSaveToConfig}
            title={dirty ? "Save changes to config.json" : "No unsaved changes"}
            style={dirty ? { boxShadow: "0 0 0 2px rgba(255,180,60,0.7)" } : {}}
          >
            {dirty ? "Save to config.json *" : "Save to config.json"}
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

      {showMasterPasswordPrompt && (
        <div className="modalOverlay">
          <div className="modalContent" style={{ maxWidth: 420 }}>
            <div className="modalHeader">
              <h2 style={{ margin: 0, fontSize: 16 }}>Master Password Required</h2>
            </div>
            <form onSubmit={(e) => { e.preventDefault(); handleMasterPasswordSubmit(); }}>
              <div className="modalBody">
                <p style={{ marginTop: 0, marginBottom: 16, color: "#ccc" }}>
                  Enter your master password to save config and encrypt the keys file.
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
                    // Success = blue, error = saturated red — blue-vs-red survives
                    // red-green color blindness where green-vs-red collapses.
                    background:
                      testStatus[idx] === "success" ? "#3b82f6"
                      : testStatus[idx] === "error" ? "#dc2626"
                      : testStatus[idx] === "testing" ? "#ffcc00"
                      : "#555",
                    boxShadow:
                      testStatus[idx] === "success" ? "0 0 6px #3b82f6"
                      : testStatus[idx] === "error" ? "0 0 6px #dc2626"
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
          className="modalOverlay"
          onMouseDown={(e) => { if (e.target === e.currentTarget) setEditing(null); }}
        >
          <div
            className="modalContent"
            style={{ maxWidth: 520 }}
          >
            <div className="modalHeader">
              <h3 style={{ margin: 0, fontSize: 15 }}>
                {editing.isNew ? "Add AI Interface" : `Edit: ${editing.originalName}`}
              </h3>
              <button className="btn" type="button" onClick={() => setEditing(null)}>×</button>
            </div>
            <div className="modalBody">
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
                <option value="API1">API1 (OpenAI chat.completions)</option>
                <option value="API2">API2 (OpenAI Responses)</option>
                <option value="API3">API3 (Gemini native)</option>
                <option value="API4">API4 (Anthropic Messages)</option>
                <option value="API5">API5 (AWS Bedrock — SigV4)</option>
                <option value="API6">API6 (Azure OpenAI — api-key)</option>
                <option value="Test">Test (no-network fixture)</option>
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

            <details style={{ marginTop: 12, marginBottom: 8 }}>
              <summary style={{ fontSize: 12, opacity: 0.85, cursor: "pointer", userSelect: "none" }}>
                Rate limit (optional — leave blank to use server defaults)
              </summary>
              <div style={{ paddingTop: 10, paddingLeft: 8, borderLeft: "2px solid rgba(255,255,255,0.1)", marginTop: 6 }}>
                <div style={{ fontSize: 11, opacity: 0.65, marginBottom: 8 }}>
                  Concurrency / retry — gates how aggressively the dispatcher fans out to this interface.
                </div>
                {field("Initial concurrency probe (default: 4–8 by interface type)", "rl_initial_concurrency_probe", "e.g. 4")}
                {field("Max concurrency (hard ceiling, default 48)", "rl_max_concurrency", "e.g. 16")}
                {field("Max 429 retries (default 10)", "rl_max_retries_429", "e.g. 10")}
                {field("Max transient retries (default 2)", "rl_max_retries_transient", "e.g. 2")}
                {field("Base retry ms (default 1000)", "rl_base_retry_ms", "e.g. 1000")}
                <div style={{ fontSize: 11, opacity: 0.65, marginBottom: 8, marginTop: 12 }}>
                  Request budget — feeds the size-aware curl timeout. See doc/jarvisagent.md for the formula.
                </div>
                {field("per_1k_input_token_seconds (default 0.5)", "rb_per_1k_input_token_seconds", "e.g. 0.5")}
                {field("per_1k_output_token_seconds (default 5.0; bump for slow models like Opus)", "rb_per_1k_output_token_seconds", "e.g. 5.0")}
                {field("fixed_overhead_seconds (default 5)", "rb_fixed_overhead_seconds", "e.g. 5")}
                {field("safety_margin_factor (default 4.0)", "rb_safety_margin_factor", "e.g. 4.0")}
                {field("min_seconds (default 60)", "rb_min_seconds", "e.g. 60")}
                {field("max_seconds (default 600)", "rb_max_seconds", "e.g. 600")}
              </div>
            </details>

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
        </div>
      )}
    </div>
  );
}
