import { useCallback, useEffect, useState } from "react";
import logoUrl from "../assets/logo.png";
import McpKeysPanel from "./McpKeysPanel";
import ScriptsPanel from "./ScriptsPanel";
import AiManagerView from "@shared/views/AiManagerView";
import ConnectionsView from "@shared/views/ConnectionsView";
import ProvidersSettingsView from "@shared/views/ProvidersSettingsView";
import {
  listAiInterfaces,
  type AiInterface,
} from "@shared/api/aiInterfaces";
import {
  getConfigSettings,
  updateConfigSettings,
  type ConfigSettings,
} from "@shared/api/configSettings";

type Tab =
  | "general"
  | "ai-interfaces"
  | "connections"
  | "keys"
  | "mcp-keys"
  | "scripts"
  | "about";

interface Props {
  onClose: () => void;
  initialTab?: Tab;
  // Role-gated tab visibility. Admin sees everything; lower roles still see
  // About (the About tab is informational and doesn't mutate anything).
  role?: string | null;
}

const TABS: Array<{ id: Tab; label: string; adminOnly?: boolean }> = [
  { id: "general",        label: "General",       adminOnly: true },
  { id: "ai-interfaces",  label: "AI Interfaces", adminOnly: true },
  { id: "connections",    label: "Connections",   adminOnly: true },
  { id: "keys",           label: "Keys",          adminOnly: true },
  { id: "mcp-keys",       label: "MCP Keys",      adminOnly: true },
  { id: "scripts",        label: "Scripts" },
  { id: "about",          label: "About" },
];

/**
 * Dashboard Settings modal — the single source of truth for global
 * configuration. The workflow editor's gear icon now holds only editor-local
 * UI preferences; everything that mutates server state (AI interfaces, keys,
 * connections, server config, MCP keys, scripts) lives here.
 */
export default function SettingsModal({ onClose, initialTab = "general", role }: Props) {
  const isAdmin = role === "admin";
  const visibleTabs = TABS.filter((t) => !t.adminOnly || isAdmin);
  const [activeTab, setActiveTab] = useState<Tab>(() =>
    visibleTabs.some((t) => t.id === initialTab) ? initialTab : (visibleTabs[0]?.id ?? "about"),
  );

  return (
    <div
      style={{
        position: "fixed",
        inset: 0,
        zIndex: 9999,
        background: "rgba(0,0,0,0.75)",
        backdropFilter: "blur(4px)",
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
      }}
      onClick={onClose}
    >
      <div
        onClick={(e) => e.stopPropagation()}
        style={{
          background: "#1e1e1e",
          border: "1px solid rgba(255,255,255,0.12)",
          borderRadius: 8,
          width: "92vw",
          maxWidth: 960,
          height: "84vh",
          display: "flex",
          flexDirection: "column",
        }}
      >
        <header
          style={{
            padding: "12px 20px",
            borderBottom: "1px solid rgba(255,255,255,0.08)",
            display: "flex",
            justifyContent: "space-between",
            alignItems: "center",
          }}
        >
          <h2 style={{ margin: 0, fontSize: 16, color: "#e8eef5" }}>Settings</h2>
          <button
            onClick={onClose}
            type="button"
            style={{
              background: "none",
              border: "none",
              color: "#e8eef5",
              fontSize: 22,
              cursor: "pointer",
              padding: "0 8px",
              lineHeight: 1,
            }}
          >
            ×
          </button>
        </header>

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
              type="button"
              onClick={() => setActiveTab(tab.id)}
              style={{
                background: activeTab === tab.id ? "#2a2a2a" : "transparent",
                color: "#e8eef5",
                border: "none",
                borderBottom:
                  activeTab === tab.id ? "2px solid #3b82f6" : "2px solid transparent",
                borderRadius: "4px 4px 0 0",
                padding: "6px 14px",
                fontSize: 13,
                cursor: "pointer",
              }}
            >
              {tab.label}
            </button>
          ))}
        </div>

        <div style={{ flex: 1, overflow: "auto", padding: 16 }}>
          {activeTab === "general" && <GeneralTab />}
          {activeTab === "ai-interfaces" && <AiManagerView appMasterPassword={null} />}
          {activeTab === "connections" && <ConnectionsView appMasterPassword={null} />}
          {activeTab === "keys" && <ProvidersSettingsView appMasterPassword={null} />}
          {activeTab === "mcp-keys" && <McpKeysPanel />}
          {activeTab === "scripts" && <ScriptsPanel />}
          {activeTab === "about" && <AboutTab />}
        </div>

        <div
          style={{
            borderTop: "1px solid rgba(255,255,255,0.08)",
            padding: "8px 16px",
            display: "flex",
            justifyContent: "flex-end",
          }}
        >
          <button
            type="button"
            onClick={onClose}
            style={{
              background: "#2a2a2a",
              color: "#e8eef5",
              border: "1px solid rgba(255,255,255,0.15)",
              borderRadius: 4,
              padding: "6px 14px",
              fontSize: 13,
              cursor: "pointer",
            }}
          >
            Close
          </button>
        </div>
      </div>
    </div>
  );
}

// =============================================================================
// General tab — server configuration (was the lower half of the editor's old
// General tab; the editor keeps only its Hide-Tier-D-warnings UI preference).
// =============================================================================

function GeneralTab() {
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
    Promise.all([getConfigSettings(), listAiInterfaces()])
      .then(([cfg, ifaces]) => {
        setServerConfig(cfg);
        setInterfaces(ifaces.interfaces);
        setDraftApiIndex(cfg.api_index);
        setDraftMaxThreads(String(cfg.max_threads));
        setDraftVerbose(cfg.verbose);
        setDraftMaxFileSize(String(cfg.max_file_size_kb));
        setDraftBatchSize(String(cfg.jcwf_batch_size));
        setDraftJcwfAiInterface(cfg.jcwf_ai_interface);
        setDraftUseBash(cfg.use_bash);
      })
      .catch(() => {
        setServerMessage("Failed to load server settings");
      });
  }, []);

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
    try {
      const result = await updateConfigSettings({
        api_index: draftApiIndex,
        max_threads: Number(draftMaxThreads),
        verbose: draftVerbose,
        max_file_size_kb: Number(draftMaxFileSize),
        jcwf_batch_size: Number(draftBatchSize),
        jcwf_ai_interface: draftJcwfAiInterface,
        use_bash: draftUseBash,
      });
      if (result.ok) {
        setServerMessage("Saved to config.json");
        const cfg = await getConfigSettings();
        setServerConfig(cfg);
      } else {
        setServerMessage(result.message ?? "Save failed");
      }
    } catch (err: unknown) {
      setServerMessage(err instanceof Error ? err.message : "Save failed");
    } finally {
      setSaving(false);
    }
  }, [draftApiIndex, draftMaxThreads, draftVerbose, draftMaxFileSize, draftBatchSize, draftJcwfAiInterface, draftUseBash]);

  const fieldRow = (label: string, content: React.ReactNode, hint?: string) => (
    <div style={{ marginBottom: 10 }}>
      <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
        <label style={{ fontSize: 13, minWidth: 160, opacity: 0.85, color: "#e8eef5" }}>{label}</label>
        {content}
      </div>
      {hint && <div style={{ fontSize: 11, color: "rgba(255,255,255,0.5)", marginTop: 2, marginLeft: 170 }}>{hint}</div>}
    </div>
  );

  return (
    <div style={{ color: "#e8eef5" }}>
      <h3 style={{ marginTop: 0, marginBottom: 12 }}>Server Configuration</h3>

      {serverConfig === null ? (
        <div style={{ color: "rgba(255,255,255,0.5)" }}>{serverMessage || "Loading..."}</div>
      ) : (
        <>
          {fieldRow("Default AI Interface",
            <select
              style={{ fontSize: 12, padding: "3px 6px", flex: 1, background: "#2a2a2a", color: "#e8eef5", border: "1px solid rgba(255,255,255,0.15)", borderRadius: 4 }}
              value={draftApiIndex}
              onChange={(e) => setDraftApiIndex(Number(e.target.value))}
            >
              {interfaces.map((iface, idx) => (
                <option key={iface.name} value={idx}>{idx}: {iface.name}</option>
              ))}
            </select>,
            "AI interface used for JCWF generation and general calls",
          )}
          {fieldRow("Max Threads",
            <input
              type="number" min={1} max={256}
              style={{ width: 70, fontSize: 12, padding: "3px 6px", background: "#2a2a2a", color: "#e8eef5", border: "1px solid rgba(255,255,255,0.15)", borderRadius: 4 }}
              value={draftMaxThreads}
              onChange={(e) => setDraftMaxThreads(e.target.value)}
            />,
            "Concurrent session manager threads (1–256)",
          )}
          {fieldRow("Max File Size (kB)",
            <input
              type="number" min={1} max={10240}
              style={{ width: 80, fontSize: 12, padding: "3px 6px", background: "#2a2a2a", color: "#e8eef5", border: "1px solid rgba(255,255,255,0.15)", borderRadius: 4 }}
              value={draftMaxFileSize}
              onChange={(e) => setDraftMaxFileSize(e.target.value)}
            />,
            "Maximum prompt file size in kB (1–10240)",
          )}
          {fieldRow("JCWF Batch Size",
            <input
              type="number" min={1} max={100}
              style={{ width: 70, fontSize: 12, padding: "3px 6px", background: "#2a2a2a", color: "#e8eef5", border: "1px solid rgba(255,255,255,0.15)", borderRadius: 4 }}
              value={draftBatchSize}
              onChange={(e) => setDraftBatchSize(e.target.value)}
            />,
            "Number of JCWF generation iterations per cycle (1\u2013100)",
          )}
          {fieldRow("JCWF AI Interface",
            <select
              style={{ fontSize: 12, padding: "3px 6px", flex: 1, background: "#2a2a2a", color: "#e8eef5", border: "1px solid rgba(255,255,255,0.15)", borderRadius: 4 }}
              value={draftJcwfAiInterface}
              onChange={(e) => setDraftJcwfAiInterface(Number(e.target.value))}
            >
              <option value={-1}>Use Default (API index {draftApiIndex})</option>
              {interfaces.map((iface, idx) => (
                <option key={iface.name} value={idx}>{idx}: {iface.name}</option>
              ))}
            </select>,
            "AI interface used for Generate / Explain / Fix Script (\u20131 = global default)",
          )}
          {fieldRow("Verbose Logging",
            <label style={{ display: "flex", alignItems: "center", gap: 6, cursor: "pointer" }}>
              <input type="checkbox" checked={draftVerbose} onChange={(e) => setDraftVerbose(e.target.checked)} />
              <span style={{ fontSize: 12 }}>Enable</span>
            </label>,
          )}
          {serverConfig.platform === "windows" && fieldRow("Use Bash (Windows)",
            <label style={{ display: "flex", alignItems: "center", gap: 6, cursor: "pointer" }}>
              <input type="checkbox" checked={draftUseBash} onChange={(e) => setDraftUseBash(e.target.checked)} />
              <span style={{ fontSize: 12 }}>Enable</span>
            </label>,
            "Use Bash (MSYS2 / Git Bash) instead of PowerShell. Requires bash on PATH; falls back to PowerShell if not found.",
          )}
          {fieldRow("Queue Folder",
            <span style={{ fontSize: 12, color: "rgba(255,255,255,0.5)" }}>{serverConfig.queue_folder}</span>,
          )}
          {fieldRow("Workflows Folder",
            <span style={{ fontSize: 12, color: "rgba(255,255,255,0.5)" }}>{serverConfig.workflows_folder}</span>,
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
            <button
              type="button"
              onClick={handleSaveServerSettings}
              disabled={!serverDirty || saving}
              style={{
                background: (!serverDirty || saving) ? "#2a2a2a" : "#3b82f6",
                color: "#e8eef5",
                border: "1px solid rgba(255,255,255,0.15)",
                borderRadius: 4,
                padding: "6px 14px",
                fontSize: 13,
                cursor: (!serverDirty || saving) ? "not-allowed" : "pointer",
                opacity: (!serverDirty || saving) ? 0.6 : 1,
              }}
            >
              {saving ? "Saving..." : "Save to config.json"}
            </button>
          </div>
        </>
      )}
    </div>
  );
}

function AboutTab() {
  return (
    <div style={{ display: "flex", alignItems: "center", gap: 16, padding: "24px 8px" }}>
      <img src={logoUrl} alt="JarvisAgent" style={{ width: 96, height: 96, borderRadius: "50%" }} />
      <div>
        <div style={{ fontSize: 18, fontWeight: 600, color: "#e8eef5" }}>JarvisAgent v0.8.5</div>
        <div style={{ fontSize: 12, color: "rgba(255,255,255,0.6)", marginTop: 6, lineHeight: 1.6 }}>
          MIT License &middot; © 2026 JC Technolabs
          <br />
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
