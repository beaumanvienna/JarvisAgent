import React, { useCallback, useEffect, useState } from "react";
import type { EditorSettings } from "../App";
import { getConfigSettings, updateConfigSettings, type ConfigSettings } from "../api/configSettings";
import { listAiInterfaces, type AiInterface } from "../api/aiInterfaces";

type SettingsModalProps = {
  settings: EditorSettings;
  onSettingsChange: (settings: EditorSettings) => void;
  onClose: () => void;
};

export default function SettingsModal({
  settings,
  onSettingsChange,
  onClose,
}: SettingsModalProps): JSX.Element
{
  const [serverConfig, setServerConfig] = useState<ConfigSettings | null>(null);
  const [interfaces, setInterfaces] = useState<AiInterface[]>([]);
  const [saving, setSaving] = useState(false);
  const [serverMessage, setServerMessage] = useState("");

  // Draft values for editable server fields
  const [draftApiIndex, setDraftApiIndex] = useState<number>(0);
  const [draftMaxThreads, setDraftMaxThreads] = useState<string>("20");
  const [draftVerbose, setDraftVerbose] = useState<boolean>(false);
  const [draftMaxFileSize, setDraftMaxFileSize] = useState<string>("24");
  const [draftBatchSize, setDraftBatchSize] = useState<string>("1");
  const [draftJcwfAiInterface, setDraftJcwfAiInterface] = useState<number>(-1);

  useEffect(() => {
    Promise.all([getConfigSettings(), listAiInterfaces()]).then(([cfg, ifaces]) => {
      setServerConfig(cfg);
      setInterfaces(ifaces.interfaces);
      setDraftApiIndex(cfg.api_index);
      setDraftMaxThreads(String(cfg.max_threads));
      setDraftVerbose(cfg.verbose);
      setDraftMaxFileSize(String(cfg.max_file_size_kb));
      setDraftBatchSize(String(cfg.jcwf_batch_size));
      setDraftJcwfAiInterface(cfg.jcwf_ai_interface);
    }).catch(() => {
      setServerMessage("Failed to load server settings");
    });
  }, []);

  const handleCheckboxChange = (key: keyof EditorSettings) => (
    e: React.ChangeEvent<HTMLInputElement>
  ) => {
    onSettingsChange({ ...settings, [key]: e.target.checked });
  };

  const serverDirty = serverConfig !== null && (
    draftApiIndex !== serverConfig.api_index ||
    Number(draftMaxThreads) !== serverConfig.max_threads ||
    draftVerbose !== serverConfig.verbose ||
    Number(draftMaxFileSize) !== serverConfig.max_file_size_kb ||
    Number(draftBatchSize) !== serverConfig.jcwf_batch_size ||
    draftJcwfAiInterface !== serverConfig.jcwf_ai_interface
  );

  const handleSaveServerSettings = useCallback(async () => {
    setSaving(true);
    setServerMessage("");
    try
    {
      const result = await updateConfigSettings({
        api_index: draftApiIndex,
        max_threads: Number(draftMaxThreads),
        verbose: draftVerbose,
        max_file_size_kb: Number(draftMaxFileSize),
        jcwf_batch_size: Number(draftBatchSize),
        jcwf_ai_interface: draftJcwfAiInterface,
      });
      if (result.ok)
      {
        setServerMessage("Saved to config.json");
        // Re-fetch to sync state
        const cfg = await getConfigSettings();
        setServerConfig(cfg);
      }
      else
      {
        setServerMessage(result.message ?? "Save failed");
      }
    }
    catch (err: unknown)
    {
      setServerMessage(err instanceof Error ? err.message : "Save failed");
    }
    finally
    {
      setSaving(false);
    }
  }, [draftApiIndex, draftMaxThreads, draftVerbose, draftMaxFileSize, draftBatchSize, draftJcwfAiInterface]);

  const fieldRow = (label: string, content: React.ReactNode, hint?: string) => (
    <div style={{ marginBottom: 10 }}>
      <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
        <label style={{ fontSize: 13, minWidth: 160, opacity: 0.85 }}>{label}</label>
        {content}
      </div>
      {hint && <div className="small muted" style={{ marginTop: 2, marginLeft: 170 }}>{hint}</div>}
    </div>
  );

  return (
    <div className="modalOverlay" onClick={onClose}>
      <div className="modalContent" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 560 }}>
        <div className="modalHeader">
          <h2 style={{ margin: 0 }}>Settings</h2>
          <button className="btn" type="button" onClick={onClose}>×</button>
        </div>

        <div className="modalBody">
          <div style={{ marginBottom: 20 }}>
            <h3 style={{ marginTop: 0, marginBottom: 8 }}>Validation Display</h3>
            <label style={{ display: "flex", alignItems: "center", gap: 8, cursor: "pointer" }}>
              <input
                type="checkbox"
                checked={settings.hideTierDWarnings}
                onChange={handleCheckboxChange("hideTierDWarnings")}
              />
              <span>Hide Tier D warnings (e.g., "working_directory is not set")</span>
            </label>
            <div className="small muted" style={{ marginTop: 4, marginLeft: 24 }}>
              Tier D are informational messages about optional fields with safe defaults.
            </div>
          </div>

          <div style={{ borderTop: "1px solid rgba(255,255,255,0.08)", paddingTop: 16 }}>
            <h3 style={{ marginTop: 0, marginBottom: 12 }}>Server Configuration</h3>

            {serverConfig === null ? (
              <div className="muted">{serverMessage || "Loading..."}</div>
            ) : (
              <>
                {fieldRow(
                  "Default AI Interface",
                  <select
                    className="input"
                    style={{ fontSize: 12, padding: "3px 6px", flex: 1 }}
                    value={draftApiIndex}
                    onChange={(e) => setDraftApiIndex(Number(e.target.value))}
                  >
                    {interfaces.map((iface, idx) => (
                      <option key={iface.name} value={idx}>
                        {idx}: {iface.name}
                      </option>
                    ))}
                  </select>,
                  "AI interface used for JCWF generation and general calls"
                )}

                {fieldRow(
                  "Max Threads",
                  <input
                    className="input"
                    type="number"
                    min={1}
                    max={256}
                    style={{ width: 70, fontSize: 12, padding: "3px 6px" }}
                    value={draftMaxThreads}
                    onChange={(e) => setDraftMaxThreads(e.target.value)}
                  />,
                  "Concurrent session manager threads (1–256)"
                )}

                {fieldRow(
                  "Max File Size (kB)",
                  <input
                    className="input"
                    type="number"
                    min={1}
                    max={10240}
                    style={{ width: 80, fontSize: 12, padding: "3px 6px" }}
                    value={draftMaxFileSize}
                    onChange={(e) => setDraftMaxFileSize(e.target.value)}
                  />,
                  "Maximum prompt file size in kB (1–10240)"
                )}

                {fieldRow(
                  "JCWF Batch Size",
                  <input
                    className="input"
                    type="number"
                    min={1}
                    max={100}
                    style={{ width: 70, fontSize: 12, padding: "3px 6px" }}
                    value={draftBatchSize}
                    onChange={(e) => setDraftBatchSize(e.target.value)}
                  />,
                  "Number of JCWF generation iterations per cycle (1\u2013100)"
                )}

                {fieldRow(
                  "JCWF AI Interface",
                  <select
                    className="input"
                    style={{ fontSize: 12, padding: "3px 6px", flex: 1 }}
                    value={draftJcwfAiInterface}
                    onChange={(e) => setDraftJcwfAiInterface(Number(e.target.value))}
                  >
                    <option value={-1}>Use Default (API index {draftApiIndex})</option>
                    {interfaces.map((iface, idx) => (
                      <option key={iface.name} value={idx}>
                        {idx}: {iface.name}
                      </option>
                    ))}
                  </select>,
                  "AI interface used for Generate / Explain / Fix Script (\u20131 = global default)"
                )}

                {fieldRow(
                  "Verbose Logging",
                  <label style={{ display: "flex", alignItems: "center", gap: 6, cursor: "pointer" }}>
                    <input
                      type="checkbox"
                      checked={draftVerbose}
                      onChange={(e) => setDraftVerbose(e.target.checked)}
                    />
                    <span style={{ fontSize: 12 }}>Enable</span>
                  </label>
                )}

                {fieldRow(
                  "Queue Folder",
                  <span className="small muted">{serverConfig.queue_folder}</span>
                )}

                {fieldRow(
                  "Workflows Folder",
                  <span className="small muted">{serverConfig.workflows_folder}</span>
                )}

                {serverMessage && (
                  <div style={{
                    marginTop: 8,
                    padding: "6px 10px",
                    borderRadius: 6,
                    fontSize: 12,
                    background: serverMessage.startsWith("Saved") ? "rgba(120,255,170,0.08)" : "rgba(255,120,120,0.08)",
                    border: serverMessage.startsWith("Saved") ? "1px solid rgba(120,255,170,0.25)" : "1px solid rgba(255,120,120,0.25)",
                  }}>
                    {serverMessage}
                  </div>
                )}

                <div style={{ marginTop: 12, display: "flex", gap: 8 }}>
                  <button
                    className="btn btnPrimary"
                    type="button"
                    onClick={handleSaveServerSettings}
                    disabled={!serverDirty || saving}
                  >
                    {saving ? "Saving..." : "Save to config.json"}
                  </button>
                </div>
              </>
            )}
          </div>
        </div>

        <div className="modalFooter">
          <button className="btn" type="button" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>
  );
}
