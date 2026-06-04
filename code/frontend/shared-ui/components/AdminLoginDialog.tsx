import { useEffect, useState } from "react";

interface Props {
  onAuthenticated: () => void;
  // Fired when the user clicks "Activate an enrollment token" OR pastes an
  // `enroll_...` token into the sign-in field. The optional prefillToken
  // carries the pasted value through so the activation dialog opens with
  // the token already filled in — zero extra clicks for the onboarding path.
  onOpenActivation?: (prefillToken?: string) => void;
  // When set, the login dialog opens with the key input prefilled.
  // Used by the activation dialog after a successful enrollment.
  prefillApiKey?: string;
}

export default function AdminLoginDialog({ onAuthenticated, onOpenActivation, prefillApiKey }: Props) {
  const [apiKey, setApiKey] = useState(prefillApiKey ?? "");
  const [error, setError] = useState("");
  const [checking, setChecking] = useState(false);
  const [showKey, setShowKey] = useState(false);

  // Keep the input in sync if the parent updates the prefill after mount
  // (e.g. the user activates a key while the login dialog is already open).
  useEffect(() => {
    if (prefillApiKey && prefillApiKey !== apiKey) {
      setApiKey(prefillApiKey);
      setShowKey(true);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [prefillApiKey]);

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    const trimmed = apiKey.trim();
    if (!trimmed) return;

    if (!trimmed.startsWith("mcp_")) {
      setError("MCP API key must start with 'mcp_'.");
      return;
    }

    setChecking(true);
    setError("");

    try {
      const res = await fetch(`${window.location.origin}/api/auth/login`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        credentials: "same-origin",
        body: JSON.stringify({ api_key: trimmed }),
      });
      if (res.ok) {
        onAuthenticated();
      } else if (res.status === 401) {
        setError("Invalid, disabled, or expired MCP key.");
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
          Sign in
        </div>
        <div style={{ fontSize: 13, color: "rgba(255,255,255,0.5)", lineHeight: 1.5 }}>
          Paste your MCP API key (starts with <code>mcp_</code>). Ask your j9t
          admin for an enrollment token if you do not have one yet.
        </div>
        <div style={{ position: "relative" }}>
          <input
            type={showKey ? "text" : "password"}
            placeholder="mcp_..."
            value={apiKey}
            onChange={(e) => {
              const next = e.target.value;
              const trimmed = next.trim();
              if (trimmed.startsWith("enroll_") && onOpenActivation)
              {
                onOpenActivation(trimmed);
                setApiKey("");
                setError("");
                return;
              }
              setApiKey(next);
            }}
            autoFocus
            style={{
              width: "100%",
              boxSizing: "border-box",
              background: "#2a2a2a",
              border: "1px solid rgba(255,255,255,0.15)",
              borderRadius: 4,
              padding: "10px 36px 10px 12px",
              color: "#e8eef5",
              fontSize: 14,
              fontFamily: "monospace",
              outline: "none",
            }}
          />
          <button
            type="button"
            onClick={() => setShowKey(!showKey)}
            tabIndex={-1}
            style={{
              position: "absolute",
              right: 8,
              top: "50%",
              transform: "translateY(-50%)",
              background: "none",
              border: "none",
              cursor: "pointer",
              padding: 4,
              fontSize: 16,
              color: "rgba(255,255,255,0.45)",
              lineHeight: 1,
            }}
            title={showKey ? "Hide key" : "Show key"}
          >
            {showKey ? "\u{1F441}" : "\u{1F441}\u{200D}\u{1F5E8}"}
          </button>
        </div>
        {error && (
          <div style={{ color: "#ff6b6b", fontSize: 13 }}>{error}</div>
        )}
        <button
          type="submit"
          disabled={checking || !apiKey.trim()}
          style={{
            background: checking ? "#333" : "#2563eb",
            color: "#fff",
            border: "none",
            borderRadius: 4,
            padding: "10px 0",
            fontSize: 14,
            fontWeight: 500,
            cursor: checking ? "wait" : "pointer",
            opacity: !apiKey.trim() ? 0.5 : 1,
          }}
        >
          {checking ? "Verifying..." : "Sign in"}
        </button>

        {onOpenActivation && (
          <div
            style={{
              marginTop: 4,
              fontSize: 12,
              color: "rgba(255,255,255,0.55)",
              textAlign: "center",
            }}
          >
            Don&apos;t have a key yet?{" "}
            <button
              type="button"
              onClick={() => onOpenActivation()}
              style={{
                background: "none",
                border: "none",
                color: "#60a5fa",
                cursor: "pointer",
                padding: 0,
                font: "inherit",
                textDecoration: "underline",
              }}
            >
              Activate an enrollment token
            </button>
          </div>
        )}
      </form>
    </div>
  );
}
