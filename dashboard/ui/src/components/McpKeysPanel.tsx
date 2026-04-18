import { useCallback, useEffect, useState } from "react";
import type { McpKeyRecord } from "../api";
import { fetchMcpKeys, revokeMcpKey } from "../api";
import EnrollmentDialog from "./EnrollmentDialog";
import ActivationDialog from "./ActivationDialog";

function formatRelative(iso: string): string {
  if (!iso) return "never";
  const t = new Date(iso).getTime();
  if (!Number.isFinite(t)) return iso;
  const deltaMs = Date.now() - t;
  if (deltaMs < 0) {
    const s = Math.round(-deltaMs / 1000);
    if (s < 60) return `in ${s}s`;
    if (s < 3600) return `in ${Math.round(s / 60)}m`;
    if (s < 86400) return `in ${Math.round(s / 3600)}h`;
    return `in ${Math.round(s / 86400)}d`;
  }
  const s = Math.round(deltaMs / 1000);
  if (s < 60) return `${s}s ago`;
  if (s < 3600) return `${Math.round(s / 60)}m ago`;
  if (s < 86400) return `${Math.round(s / 3600)}h ago`;
  return `${Math.round(s / 86400)}d ago`;
}

function statusColor(key: McpKeyRecord): string {
  if (!key.enabled) return "#64748b";
  const exp = new Date(key.expires_at).getTime();
  if (Number.isFinite(exp) && exp < Date.now()) return "#ef4444";
  return "#22c55e";
}

export default function McpKeysPanel() {
  const [keys, setKeys] = useState<McpKeyRecord[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [showEnroll, setShowEnroll] = useState(false);
  const [showActivate, setShowActivate] = useState(false);

  const reload = useCallback(async () => {
    setError(null);
    try {
      const resp = await fetchMcpKeys();
      if (resp.ok) {
        setKeys(resp.keys);
      } else {
        setError(resp.error || "Failed to load MCP keys");
        setKeys([]);
      }
    } catch (e) {
      setError("Network error loading MCP keys");
      setKeys([]);
    }
  }, []);

  useEffect(() => {
    reload();
  }, [reload]);

  const handleRevoke = async (keyId: string) => {
    if (!window.confirm(`Revoke ${keyId}? This cannot be undone.`)) return;
    await revokeMcpKey(keyId);
    reload();
  };

  const cell: React.CSSProperties = {
    padding: "6px 10px",
    borderBottom: "1px solid rgba(255,255,255,0.08)",
    fontSize: 13,
    color: "#e8eef5",
  };
  const headCell: React.CSSProperties = {
    ...cell,
    fontWeight: 600,
    color: "rgba(255,255,255,0.65)",
    background: "#181b20",
    position: "sticky",
    top: 0,
  };

  return (
    <section
      style={{
        padding: 16,
        display: "flex",
        flexDirection: "column",
        gap: 12,
        background: "#141619",
        borderRadius: 6,
        border: "1px solid rgba(255,255,255,0.08)",
      }}
    >
      <header
        style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}
      >
        <div>
          <h2 style={{ margin: 0, fontSize: 16, color: "#e8eef5" }}>MCP API Keys</h2>
          <p style={{ margin: "4px 0 0", fontSize: 12, color: "rgba(255,255,255,0.5)" }}>
            Issue enrollment tokens — users activate them to receive their own API key.
          </p>
        </div>
        <div style={{ display: "flex", gap: 8 }}>
          <button
            onClick={() => setShowActivate(true)}
            title="Paste an enrollment token (enroll_...) to receive your MCP API key"
            style={{
              background: "#2a2a2a",
              color: "#e8eef5",
              border: "1px solid rgba(255,255,255,0.15)",
              borderRadius: 4,
              padding: "8px 14px",
              fontSize: 13,
              cursor: "pointer",
            }}
          >
            Activate enrollment
          </button>
          <button
            onClick={() => setShowEnroll(true)}
            style={{
              background: "#2563eb",
              color: "#fff",
              border: "none",
              borderRadius: 4,
              padding: "8px 14px",
              fontSize: 13,
              cursor: "pointer",
            }}
          >
            + Create enrollment
          </button>
        </div>
      </header>

      {error && (
        <div
          style={{
            padding: 10,
            background: "rgba(239,68,68,0.12)",
            border: "1px solid rgba(239,68,68,0.4)",
            color: "#ff6b6b",
            fontSize: 13,
            borderRadius: 4,
          }}
        >
          {error}
        </div>
      )}

      <div style={{ maxHeight: 480, overflowY: "auto", border: "1px solid rgba(255,255,255,0.08)", borderRadius: 4 }}>
        <table style={{ width: "100%", borderCollapse: "collapse" }}>
          <thead>
            <tr>
              <th style={{ ...headCell, width: 24 }}></th>
              <th style={headCell}>Key ID</th>
              <th style={headCell}>User</th>
              <th style={headCell}>Role</th>
              <th style={headCell}>Adhoc</th>
              <th style={headCell}>Quota</th>
              <th style={headCell}>Last used</th>
              <th style={headCell}>Expires</th>
              <th style={{ ...headCell, width: 80 }}>Actions</th>
            </tr>
          </thead>
          <tbody>
            {keys === null && (
              <tr>
                <td colSpan={9} style={{ ...cell, color: "rgba(255,255,255,0.5)" }}>
                  Loading...
                </td>
              </tr>
            )}
            {keys !== null && keys.length === 0 && (
              <tr>
                <td colSpan={9} style={{ ...cell, color: "rgba(255,255,255,0.5)" }}>
                  No MCP keys yet. Create an enrollment to issue the first key.
                </td>
              </tr>
            )}
            {keys?.map((k) => (
              <tr key={k.key_id}>
                <td style={cell}>
                  <span
                    style={{
                      display: "inline-block",
                      width: 10,
                      height: 10,
                      borderRadius: "50%",
                      background: statusColor(k),
                    }}
                  />
                </td>
                <td style={{ ...cell, fontFamily: "monospace" }}>{k.key_id}</td>
                <td style={cell}>{k.user}</td>
                <td style={cell}>
                  <span
                    style={{
                      padding: "2px 6px",
                      borderRadius: 3,
                      fontSize: 11,
                      background:
                        k.role === "admin"
                          ? "#7c3aed"
                          : k.role === "operator"
                          ? "#2563eb"
                          : "#64748b",
                      color: "#fff",
                    }}
                  >
                    {k.role}
                  </span>
                </td>
                <td style={cell}>{k.adhoc_enabled ? "yes" : "no"}</td>
                <td style={cell}>{k.disk_quota_mb} MB</td>
                <td style={cell}>{formatRelative(k.last_used_at)}</td>
                <td style={cell}>{formatRelative(k.expires_at)}</td>
                <td style={cell}>
                  <button
                    onClick={() => handleRevoke(k.key_id)}
                    style={{
                      background: "none",
                      border: "1px solid rgba(239,68,68,0.6)",
                      color: "#ef4444",
                      borderRadius: 3,
                      padding: "2px 8px",
                      fontSize: 11,
                      cursor: "pointer",
                    }}
                  >
                    Revoke
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {showEnroll && (
        <EnrollmentDialog
          onClose={() => {
            setShowEnroll(false);
            reload();
          }}
        />
      )}

      {showActivate && (
        <ActivationDialog
          onClose={() => {
            setShowActivate(false);
            reload();
          }}
          onActivated={() => {
            // Key issued — reload the list so the new key shows up.
            setShowActivate(false);
            reload();
          }}
        />
      )}
    </section>
  );
}
