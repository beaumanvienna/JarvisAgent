import { useState } from "react";
import McpKeysPanel from "./McpKeysPanel";
import ScriptsPanel from "./ScriptsPanel";

type Tab = "mcp-keys" | "scripts" | "about";

interface Props {
  onClose: () => void;
  // Role-gated tab visibility. Admin sees everything; lower roles still see
  // About (the About tab is informational and doesn't mutate anything).
  role?: string | null;
}

const TABS: Array<{ id: Tab; label: string; adminOnly?: boolean }> = [
  { id: "mcp-keys", label: "MCP Keys", adminOnly: true },
  { id: "scripts",  label: "Scripts" },
  { id: "about",    label: "About" },
];

/**
 * Dashboard Settings modal — mirrors the workflow editor's gear-icon UX so the
 * two React apps feel consistent. MCP Keys management lives here (single source
 * of truth; the editor's Dashboard button reaches it in one click for Studio
 * users). About is present for symmetry with the editor.
 */
export default function SettingsModal({ onClose, role }: Props) {
  const isAdmin = role === "admin";
  const visibleTabs = TABS.filter((t) => !t.adminOnly || isAdmin);
  const [activeTab, setActiveTab] = useState<Tab>(visibleTabs[0]?.id ?? "about");

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

function AboutTab() {
  return (
    <div style={{ display: "flex", alignItems: "center", gap: 16, padding: 24 }}>
      <div>
        <div style={{ fontSize: 18, fontWeight: 600, color: "#e8eef5" }}>JarvisAgent</div>
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
