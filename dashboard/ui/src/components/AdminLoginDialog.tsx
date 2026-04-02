import { useState } from "react";
import { setToken } from "../auth";

interface Props {
  onAuthenticated: () => void;
}

export default function AdminLoginDialog({ onAuthenticated }: Props) {
  const [token, setTokenInput] = useState("");
  const [error, setError] = useState("");
  const [checking, setChecking] = useState(false);

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!token.trim()) return;

    setChecking(true);
    setError("");

    try {
      // Test the token against a protected endpoint.
      const res = await fetch(`${window.location.origin}/api/workflows`, {
        headers: { Authorization: `Bearer ${token.trim()}` },
      });

      if (res.ok) {
        setToken(token.trim());
        onAuthenticated();
      } else if (res.status === 403) {
        setError("Invalid token.");
      } else {
        setError(`Unexpected response: ${res.status}`);
      }
    } catch {
      setError("Cannot reach server.");
    } finally {
      setChecking(false);
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
      <form
        onSubmit={handleSubmit}
        style={{
          background: "#1e1e1e",
          border: "1px solid rgba(255,255,255,0.12)",
          borderRadius: 8,
          padding: "32px 28px",
          width: 380,
          display: "flex",
          flexDirection: "column",
          gap: 16,
        }}
      >
        <div style={{ fontSize: 18, fontWeight: 600, color: "#e8eef5" }}>
          Admin Login
        </div>
        <div style={{ fontSize: 13, color: "rgba(255,255,255,0.5)", lineHeight: 1.5 }}>
          This JarvisAgent instance requires authentication.
          Enter the admin API token from config.json.
        </div>
        <input
          type="password"
          placeholder="API token"
          value={token}
          onChange={(e) => setTokenInput(e.target.value)}
          autoFocus
          style={{
            background: "#2a2a2a",
            border: "1px solid rgba(255,255,255,0.15)",
            borderRadius: 4,
            padding: "10px 12px",
            color: "#e8eef5",
            fontSize: 14,
            fontFamily: "monospace",
            outline: "none",
          }}
        />
        {error && (
          <div style={{ color: "#ff6b6b", fontSize: 13 }}>{error}</div>
        )}
        <button
          type="submit"
          disabled={checking || !token.trim()}
          style={{
            background: checking ? "#333" : "#2563eb",
            color: "#fff",
            border: "none",
            borderRadius: 4,
            padding: "10px 0",
            fontSize: 14,
            fontWeight: 500,
            cursor: checking ? "wait" : "pointer",
            opacity: !token.trim() ? 0.5 : 1,
          }}
        >
          {checking ? "Verifying..." : "Login"}
        </button>
      </form>
    </div>
  );
}
