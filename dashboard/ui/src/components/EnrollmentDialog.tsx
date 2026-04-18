import { useState } from "react";
import { createMcpEnrollment } from "../api";

interface Props {
  onClose: () => void;
}

type Role = "admin" | "operator" | "viewer";

export default function EnrollmentDialog({ onClose }: Props) {
  const [user, setUser] = useState("");
  const [role, setRole] = useState<Role>("operator");
  const [adhocEnabled, setAdhocEnabled] = useState(false);
  const [diskQuotaMb, setDiskQuotaMb] = useState(1024);
  const [cleanupPolicy, setCleanupPolicy] = useState("ttl_72h");
  const [keyExpiryDays, setKeyExpiryDays] = useState(90);
  const [description, setDescription] = useState("");
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [token, setToken] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!user.trim()) {
      setError("User is required.");
      return;
    }
    setSubmitting(true);
    setError(null);
    try {
      const resp = await createMcpEnrollment({
        user: user.trim(),
        role,
        adhoc_enabled: adhocEnabled,
        disk_quota_mb: diskQuotaMb,
        default_cleanup_policy: cleanupPolicy,
        description,
        key_expiry_days: keyExpiryDays,
        enrollment_ttl_minutes: 30,
      });
      if (!resp.ok) {
        setError("Enrollment request failed.");
      } else {
        setToken(resp.enrollment_token);
      }
    } catch {
      setError("Network error.");
    } finally {
      setSubmitting(false);
    }
  };

  const copy = async () => {
    if (!token) return;
    try {
      await navigator.clipboard.writeText(token);
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    } catch {
      setCopied(false);
    }
  };

  return (
    <div
      style={{
        position: "fixed",
        inset: 0,
        zIndex: 9999,
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        background: "rgba(0,0,0,0.75)",
        backdropFilter: "blur(4px)",
      }}
    >
      <div
        style={{
          background: "#1e1e1e",
          border: "1px solid rgba(255,255,255,0.12)",
          borderRadius: 8,
          padding: 24,
          width: 480,
          maxWidth: "90vw",
          display: "flex",
          flexDirection: "column",
          gap: 12,
        }}
      >
        {token ? (
          <>
            <div style={{ fontSize: 18, fontWeight: 600, color: "#e8eef5" }}>
              Enrollment created
            </div>
            <div style={{ fontSize: 13, color: "rgba(255,255,255,0.65)", lineHeight: 1.5 }}>
              Share this token with <strong>{user}</strong> through a secure channel.
              It expires in 30 minutes and is single-use. You will <em>not</em>{" "}
              see their final API key — they exchange this token for it at{" "}
              <code>POST /api/auth/mcp-keys/activate</code>.
            </div>
            <div
              style={{
                background: "#2a2a2a",
                border: "1px solid rgba(255,255,255,0.15)",
                borderRadius: 4,
                padding: "10px 12px",
                fontFamily: "monospace",
                fontSize: 12,
                color: "#e8eef5",
                wordBreak: "break-all",
                userSelect: "all",
              }}
            >
              {token}
            </div>
            <div style={{ display: "flex", gap: 8 }}>
              <button
                onClick={copy}
                style={{
                  flex: 1,
                  background: copied ? "#16a34a" : "#2563eb",
                  color: "#fff",
                  border: "none",
                  borderRadius: 4,
                  padding: "8px 0",
                  fontSize: 13,
                  cursor: "pointer",
                }}
              >
                {copied ? "Copied" : "Copy to clipboard"}
              </button>
              <button
                onClick={onClose}
                style={{
                  flex: 1,
                  background: "#2a2a2a",
                  color: "#e8eef5",
                  border: "1px solid rgba(255,255,255,0.15)",
                  borderRadius: 4,
                  padding: "8px 0",
                  fontSize: 13,
                  cursor: "pointer",
                }}
              >
                Done
              </button>
            </div>
          </>
        ) : (
          <form onSubmit={handleSubmit} style={{ display: "flex", flexDirection: "column", gap: 12 }}>
            <div style={{ fontSize: 18, fontWeight: 600, color: "#e8eef5" }}>
              Create MCP enrollment
            </div>
            <label style={labelStyle}>
              User (email or username)
              <input
                type="text"
                value={user}
                onChange={(e) => setUser(e.target.value)}
                required
                autoFocus
                style={inputStyle}
              />
            </label>
            <label style={labelStyle}>
              Role
              <select
                value={role}
                onChange={(e) => setRole(e.target.value as Role)}
                style={inputStyle}
              >
                <option value="viewer">viewer</option>
                <option value="operator">operator</option>
                <option value="admin">admin</option>
              </select>
            </label>
            <label style={{ ...labelStyle, flexDirection: "row", alignItems: "center", gap: 8 }}>
              <input
                type="checkbox"
                checked={adhocEnabled}
                onChange={(e) => setAdhocEnabled(e.target.checked)}
              />
              Allow adhoc workflow submission
            </label>
            <div style={{ display: "flex", gap: 10 }}>
              <label style={{ ...labelStyle, flex: 1 }}>
                Disk quota (MB)
                <input
                  type="number"
                  min={1}
                  value={diskQuotaMb}
                  onChange={(e) => setDiskQuotaMb(Number(e.target.value))}
                  style={inputStyle}
                />
              </label>
              <label style={{ ...labelStyle, flex: 1 }}>
                Key expires (days)
                <input
                  type="number"
                  min={1}
                  max={365}
                  value={keyExpiryDays}
                  onChange={(e) => setKeyExpiryDays(Number(e.target.value))}
                  style={inputStyle}
                />
              </label>
            </div>
            <label style={labelStyle}>
              Default cleanup policy
              <select
                value={cleanupPolicy}
                onChange={(e) => setCleanupPolicy(e.target.value)}
                style={inputStyle}
              >
                <option value="on_completion">on_completion</option>
                <option value="ttl_1h">ttl_1h</option>
                <option value="ttl_24h">ttl_24h</option>
                <option value="ttl_48h">ttl_48h</option>
                <option value="ttl_72h">ttl_72h (default)</option>
                <option value="retain">retain</option>
              </select>
            </label>
            <label style={labelStyle}>
              Description (optional)
              <input
                type="text"
                value={description}
                onChange={(e) => setDescription(e.target.value)}
                style={inputStyle}
              />
            </label>

            {error && <div style={{ color: "#ff6b6b", fontSize: 13 }}>{error}</div>}

            <div style={{ display: "flex", gap: 8 }}>
              <button
                type="submit"
                disabled={submitting}
                style={{
                  flex: 1,
                  background: submitting ? "#333" : "#2563eb",
                  color: "#fff",
                  border: "none",
                  borderRadius: 4,
                  padding: "10px 0",
                  fontSize: 14,
                  cursor: submitting ? "wait" : "pointer",
                }}
              >
                {submitting ? "Creating..." : "Create enrollment"}
              </button>
              <button
                type="button"
                onClick={onClose}
                style={{
                  flex: 1,
                  background: "#2a2a2a",
                  color: "#e8eef5",
                  border: "1px solid rgba(255,255,255,0.15)",
                  borderRadius: 4,
                  padding: "10px 0",
                  fontSize: 14,
                  cursor: "pointer",
                }}
              >
                Cancel
              </button>
            </div>
          </form>
        )}
      </div>
    </div>
  );
}

const labelStyle: React.CSSProperties = {
  display: "flex",
  flexDirection: "column",
  gap: 4,
  fontSize: 12,
  color: "rgba(255,255,255,0.7)",
};

const inputStyle: React.CSSProperties = {
  background: "#2a2a2a",
  border: "1px solid rgba(255,255,255,0.15)",
  borderRadius: 4,
  padding: "8px 10px",
  color: "#e8eef5",
  fontSize: 13,
  outline: "none",
};
