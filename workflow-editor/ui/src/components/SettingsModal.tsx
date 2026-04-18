import React, { useCallback, useEffect, useState } from "react";
import type { EditorSettings } from "../App";
import { getConfigSettings, updateConfigSettings, type ConfigSettings } from "../api/configSettings";
import { listAiInterfaces, type AiInterface } from "../api/aiInterfaces";
import AiManagerView from "../views/AiManagerView";
import ConnectionsView from "../views/ConnectionsView";
import ProvidersSettingsView from "../views/ProvidersSettingsView";
import logoUrl from "../assets/logo.png";

// MCP Keys management lives in the dashboard, not here — deduplicated so there's
// one canonical place to maintain the enrollment / list / revoke flow. Users in
// the editor use the "Dashboard" button in the top nav to reach it.
export type SettingsTab =
  | "general"
  | "ai-interfaces"
  | "connections"
  | "keys"
  | "about";

type SettingsModalProps = {
  settings: EditorSettings;
  onSettingsChange: (settings: EditorSettings) => void;
  onClose: () => void;
  initialTab?: SettingsTab;
  masterPassword: string | null;
  role?: "admin" | "operator" | "viewer";
  onAiManagerDirty?: (dirty: boolean) => void;
  onKeysDirty?: (dirty: boolean) => void;
  onConnectionsDirty?: (dirty: boolean) => void;
};

// General and About are available to everyone; the rest mutate server state
// and require admin. The backend also enforces this — UI gating is cosmetic.
const TABS: Array<{ id: SettingsTab; label: string; adminOnly?: boolean }> = [
  { id: "general",        label: "General" },
  { id: "ai-interfaces",  label: "AI Interfaces", adminOnly: true },
  { id: "connections",    label: "Connections",   adminOnly: true },
  { id: "keys",           label: "Keys",          adminOnly: true },
  { id: "about",          label: "About" },
];

export default function SettingsModal({
  settings,
  onSettingsChange,
  onClose,
  initialTab = "general",
  masterPassword,
  role = "admin",
  onAiManagerDirty,
  onKeysDirty,
  onConnectionsDirty,
}: SettingsModalProps): JSX.Element
{
  const visibleTabs = TABS.filter((t) => !t.adminOnly || role === "admin");
  const [activeTab, setActiveTab] = useState<SettingsTab>(() =>
    visibleTabs.some((t) => t.id === initialTab) ? initialTab : "general"
  );

  return (
    <div className="modalOverlay" onClick={onClose}>
      <div
        className="modalContent"
        onClick={(e) => e.stopPropagation()}
        style={{ maxWidth: 960, width: "92vw", height: "84vh", display: "flex", flexDirection: "column" }}
      >
        <div className="modalHeader">
          <h2 style={{ margin: 0 }}>Settings</h2>
          <button className="btn" type="button" onClick={onClose}>×</button>
        </div>

        <div
          style={{
            display: "flex",
            gap: 4,
            padding: "8px 16px 0",
            borderBottom: "1px solid rgba(255,255,255,0.08)",
            flexWrap: "wrap",
          }}
        >
          {visibleTabs.map((tab) => (
            <button
              key={tab.id}
              className={`btn ${activeTab === tab.id ? "btnActive" : ""}`}
              type="button"
              onClick={() => setActiveTab(tab.id)}
              style={{
                borderBottom: activeTab === tab.id ? "2px solid #3b82f6" : "2px solid transparent",
                borderRadius: "4px 4px 0 0",
                padding: "6px 14px",
                fontSize: 13,
              }}
            >
              {tab.label}
            </button>
          ))}
        </div>

        <div className="modalBody" style={{ flex: 1, overflow: "auto" }}>
          {activeTab === "general" && (
            <GeneralTab settings={settings} onSettingsChange={onSettingsChange} />
          )}
          {activeTab === "ai-interfaces" && (
            <AiManagerView appMasterPassword={masterPassword} onDirtyStateChange={onAiManagerDirty} />
          )}
          {activeTab === "connections" && (
            <ConnectionsView appMasterPassword={masterPassword} onDirtyStateChange={onConnectionsDirty} />
          )}
          {activeTab === "keys" && (
            <ProvidersSettingsView appMasterPassword={masterPassword} onDirtyStateChange={onKeysDirty} />
          )}
          {activeTab === "about" && <AboutTab />}
        </div>

        <div style={{ borderTop: "1px solid rgba(255,255,255,0.08)", padding: "8px 16px", display: "flex", justifyContent: "flex-end" }}>
          <button className="btn" type="button" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>
  );
}

// =============================================================================
// General tab — validation display + server configuration (the old modal body)
// =============================================================================

function GeneralTab({
  settings,
  onSettingsChange,
}: {
  settings: EditorSettings;
  onSettingsChange: (settings: EditorSettings) => void;
}): JSX.Element
{
  const [serverConfig, setServerConfig] = useState<ConfigSettings | null>(null);
  const [interfaces, setInterfaces] = useState<AiInterface[]>([]);
  const [saving, setSaving] = useState(false);
  const [serverMessage, setServerMessage] = useState("");

  const [draftApiIndex, setDraftApiIndex] = useState<number>(0);
  const [draftMaxThreads, setDraftMaxThreads] = useState<string>("20");
  const [draftVerbose, setDraftVerbose] = useState<boolean>(false);
  const [draftMaxFileSize, setDraftMaxFileSize] = useState<string>("24");
  const [draftBatchSize, setDraftBatchSize] = useState<string>("1");
  const [draftJcwfAiInterface, setDraftJcwfAiInterface] = useState<number>(-1);
  const [draftUseBash, setDraftUseBash] = useState<boolean>(false);

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
      setDraftUseBash(cfg.use_bash);
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
    draftJcwfAiInterface !== serverConfig.jcwf_ai_interface ||
    draftUseBash !== serverConfig.use_bash
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
        use_bash: draftUseBash,
      });
      if (result.ok)
      {
        setServerMessage("Saved to config.json");
        const cfg = await getConfigSettings();
        setServerConfig(cfg);
      }
      else setServerMessage(result.message ?? "Save failed");
    }
    catch (err: unknown)
    {
      setServerMessage(err instanceof Error ? err.message : "Save failed");
    }
    finally
    {
      setSaving(false);
    }
  }, [draftApiIndex, draftMaxThreads, draftVerbose, draftMaxFileSize, draftBatchSize, draftJcwfAiInterface, draftUseBash]);

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
    <>
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
            {fieldRow("Default AI Interface",
              <select className="input" style={{ fontSize: 12, padding: "3px 6px", flex: 1 }}
                      value={draftApiIndex}
                      onChange={(e) => setDraftApiIndex(Number(e.target.value))}>
                {interfaces.map((iface, idx) => (
                  <option key={iface.name} value={idx}>{idx}: {iface.name}</option>
                ))}
              </select>,
              "AI interface used for JCWF generation and general calls"
            )}
            {fieldRow("Max Threads",
              <input className="input" type="number" min={1} max={256}
                     style={{ width: 70, fontSize: 12, padding: "3px 6px" }}
                     value={draftMaxThreads}
                     onChange={(e) => setDraftMaxThreads(e.target.value)} />,
              "Concurrent session manager threads (1–256)"
            )}
            {fieldRow("Max File Size (kB)",
              <input className="input" type="number" min={1} max={10240}
                     style={{ width: 80, fontSize: 12, padding: "3px 6px" }}
                     value={draftMaxFileSize}
                     onChange={(e) => setDraftMaxFileSize(e.target.value)} />,
              "Maximum prompt file size in kB (1–10240)"
            )}
            {fieldRow("JCWF Batch Size",
              <input className="input" type="number" min={1} max={100}
                     style={{ width: 70, fontSize: 12, padding: "3px 6px" }}
                     value={draftBatchSize}
                     onChange={(e) => setDraftBatchSize(e.target.value)} />,
              "Number of JCWF generation iterations per cycle (1\u2013100)"
            )}
            {fieldRow("JCWF AI Interface",
              <select className="input" style={{ fontSize: 12, padding: "3px 6px", flex: 1 }}
                      value={draftJcwfAiInterface}
                      onChange={(e) => setDraftJcwfAiInterface(Number(e.target.value))}>
                <option value={-1}>Use Default (API index {draftApiIndex})</option>
                {interfaces.map((iface, idx) => (
                  <option key={iface.name} value={idx}>{idx}: {iface.name}</option>
                ))}
              </select>,
              "AI interface used for Generate / Explain / Fix Script (\u20131 = global default)"
            )}
            {fieldRow("Verbose Logging",
              <label style={{ display: "flex", alignItems: "center", gap: 6, cursor: "pointer" }}>
                <input type="checkbox" checked={draftVerbose} onChange={(e) => setDraftVerbose(e.target.checked)} />
                <span style={{ fontSize: 12 }}>Enable</span>
              </label>
            )}
            {serverConfig.platform === "windows" && fieldRow("Use Bash (Windows)",
              <label style={{ display: "flex", alignItems: "center", gap: 6, cursor: "pointer" }}>
                <input type="checkbox" checked={draftUseBash} onChange={(e) => setDraftUseBash(e.target.checked)} />
                <span style={{ fontSize: 12 }}>Enable</span>
              </label>,
              "Use Bash (MSYS2 / Git Bash) instead of PowerShell. Requires bash on PATH; falls back to PowerShell if not found."
            )}
            {fieldRow("Queue Folder",
              <span className="small muted">{serverConfig.queue_folder}</span>
            )}
            {fieldRow("Workflows Folder",
              <span className="small muted">{serverConfig.workflows_folder}</span>
            )}

            {serverMessage && (
              <div style={{
                marginTop: 8, padding: "6px 10px", borderRadius: 6, fontSize: 12,
                background: serverMessage.startsWith("Saved") ? "rgba(120,255,170,0.08)" : "rgba(255,120,120,0.08)",
                border: serverMessage.startsWith("Saved") ? "1px solid rgba(120,255,170,0.25)" : "1px solid rgba(255,120,120,0.25)",
              }}>
                {serverMessage}
              </div>
            )}

            <div style={{ marginTop: 12, display: "flex", gap: 8 }}>
              <button className="btn btnPrimary" type="button"
                      onClick={handleSaveServerSettings}
                      disabled={!serverDirty || saving}>
                {saving ? "Saving..." : "Save to config.json"}
              </button>
            </div>
          </>
        )}
      </div>
    </>
  );
}

// =============================================================================
// About tab
// =============================================================================

function AboutTab(): JSX.Element
{
  return (
    <div style={{ display: "flex", alignItems: "center", gap: 16, padding: "24px 8px" }}>
      <img src={logoUrl} alt="JarvisAgent" style={{ width: 96, height: 96, borderRadius: "50%" }} />
      <div>
        <div style={{ fontSize: 18, fontWeight: 600 }}>JarvisAgent v0.8.5</div>
        <div style={{ fontSize: 12, opacity: 0.7, marginTop: 6, lineHeight: 1.6 }}>
          MIT License &middot; &copy; 2026 JC Technolabs<br />
          <a
            href="https://github.com/beaumanvienna/JarvisAgent"
            target="_blank"
            rel="noopener noreferrer"
            style={{ color: "rgba(120,180,255,0.85)", textDecoration: "none" }}
          >
            github.com/beaumanvienna/JarvisAgent
          </a>
        </div>
      </div>
    </div>
  );
}
