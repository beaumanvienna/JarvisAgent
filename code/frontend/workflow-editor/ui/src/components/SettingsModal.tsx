import React, { useState } from "react";
import type { EditorSettings } from "../App";
import logoUrl from "../assets/logo.png";

// Global configuration (AI Interfaces, Connections, Keys, server config) has
// moved to the dashboard app. This modal now holds only editor-local UI
// preferences and the About tab.
export type SettingsTab = "general" | "about";

type SettingsModalProps = {
  settings: EditorSettings;
  onSettingsChange: (settings: EditorSettings) => void;
  onClose: () => void;
  initialTab?: SettingsTab;
};

const TABS: Array<{ id: SettingsTab; label: string }> = [
  { id: "general", label: "General" },
  { id: "about",   label: "About" },
];

export default function SettingsModal({
  settings,
  onSettingsChange,
  onClose,
  initialTab = "general",
}: SettingsModalProps): JSX.Element
{
  const [activeTab, setActiveTab] = useState<SettingsTab>(initialTab);

  return (
    <div className="modalOverlay" onClick={onClose}>
      <div
        className="modalContent"
        onClick={(e) => e.stopPropagation()}
        style={{ maxWidth: 640, width: "80vw", height: "60vh", display: "flex", flexDirection: "column" }}
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
          {TABS.map((tab) => (
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
// General tab — editor-local UI preferences (validation display filter).
// Global settings (server config, AI interfaces, keys, connections) live in the
// dashboard app's Settings modal.
// =============================================================================

function GeneralTab({
  settings,
  onSettingsChange,
}: {
  settings: EditorSettings;
  onSettingsChange: (settings: EditorSettings) => void;
}): JSX.Element
{
  const handleCheckboxChange = (key: keyof EditorSettings) => (
    e: React.ChangeEvent<HTMLInputElement>
  ) => {
    onSettingsChange({ ...settings, [key]: e.target.checked });
  };

  return (
    <div style={{ padding: "8px 0" }}>
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

      <div
        className="small muted"
        style={{
          marginTop: 24,
          padding: "10px 12px",
          border: "1px solid rgba(255,255,255,0.08)",
          borderRadius: 6,
        }}
      >
        Global configuration — AI interfaces, API keys, connections, and server
        settings — can be found in the <strong>Dashboard</strong>. Click the
        <strong>&nbsp;Dashboard&nbsp;</strong>button in the top bar and open its
        Settings gear.
      </div>
    </div>
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
        <div style={{ fontSize: 18, fontWeight: 600 }}>JarvisAgent v0.8.7</div>
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
