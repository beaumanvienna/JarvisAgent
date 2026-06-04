import { useState } from "react";

interface Props {
  onClose: () => void;
  // Called when the user clicks "Use this key to sign in". The parent
  // prefills the login dialog with the freshly-issued mcp_ key.
  onActivated: (apiKey: string) => void;
  // Prefill when the sign-in dialog auto-switched after detecting an
  // `enroll_...` paste — the user shouldn't have to paste twice.
  prefillToken?: string;
}

/**
 * First-run / onboarding dialog.
 *
 * The admin reads the enrollment token (`enroll_...`) from j9t's startup
 * banner in `log/log.txt` or the TUI, pastes it here, and receives the real
 * MCP API key. The key is shown exactly once — the dialog highlights this
 * and offers a copy button plus a "sign in" handoff.
 */
export default function ActivationDialog({ onClose, onActivated, prefillToken }: Props) {
  const [token, setTokenInput] = useState(prefillToken ?? "");
  const [error, setError] = useState("");
  const [submitting, setSubmitting] = useState(false);
  const [issuedKey, setIssuedKey] = useState<string | null>(null);
  const [issuedRole, setIssuedRole] = useState<string>("");
  const [copied, setCopied] = useState(false);
  // Explicit "I saved the key" acknowledgement. The issued key is shown exactly
  // once — the user must confirm they've stored it somewhere durable before we
  // let them leave the dialog, otherwise they'd have to re-enroll.
  const [savedAck, setSavedAck] = useState(false);

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    const trimmed = token.trim();
    if (!trimmed) return;
    if (!trimmed.startsWith("enroll_")) {
      setError("Enrollment tokens start with 'enroll_'. Check the j9t log banner.");
      return;
    }

    setSubmitting(true);
    setError("");
    try {
      const res = await fetch(
        `${window.location.origin}/api/auth/mcp-keys/activate`,
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          credentials: "same-origin",
          body: JSON.stringify({ enrollment_token: trimmed }),
        }
      );
      if (res.ok) {
        const body = await res.json();
        setIssuedKey(body.api_key ?? null);
        setIssuedRole(body.role ?? "");
      } else if (res.status === 401) {
        setError("This enrollment token is invalid or has expired.");
      } else if (res.status === 503) {
        setError("MCP key store is not unlocked yet. Restart j9t to get a fresh bootstrap token.");
      } else {
        setError(`Unexpected response: ${res.status}`);
      }
    } catch {
      setError("Cannot reach server.");
    } finally {
      setSubmitting(false);
    }
  };

  const copy = async () => {
    if (!issuedKey) return;
    try {
      await navigator.clipboard.writeText(issuedKey);
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
        zIndex: 10000,
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
          padding: "32px 28px",
          width: 460,
          maxWidth: "92vw",
          display: "flex",
          flexDirection: "column",
          gap: 14,
        }}
      >
        <div style={{ fontSize: 18, fontWeight: 600, color: "#e8eef5" }}>
          {issuedKey ? "Your MCP API key" : "Activate enrollment token"}
        </div>

        {issuedKey ? (
          <>
            <div style={{ fontSize: 13, color: "rgba(255,255,255,0.65)", lineHeight: 1.5 }}>
              This is your new MCP API key — <strong>it is shown exactly once</strong>.
              Save it to a password manager or your Claude Code config before closing
              this dialog. The role is <code>{issuedRole || "unknown"}</code>.
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
              {issuedKey}
            </div>
            <label
              style={{
                display: "flex",
                alignItems: "center",
                gap: 10,
                padding: "10px 12px",
                background: savedAck ? "rgba(34,197,94,0.10)" : "rgba(239,68,68,0.10)",
                border: savedAck
                  ? "1px solid rgba(34,197,94,0.45)"
                  : "1px solid rgba(239,68,68,0.45)",
                borderRadius: 4,
                cursor: "pointer",
                userSelect: "none",
                fontSize: 13,
                color: "#e8eef5",
                lineHeight: 1.4,
              }}
            >
              <input
                type="checkbox"
                checked={savedAck}
                onChange={(e) => setSavedAck(e.target.checked)}
                style={{ accentColor: savedAck ? "#22c55e" : "#ef4444", flexShrink: 0 }}
              />
              <span>
                I have saved this MCP API key to a password manager or my Claude
                Code config. I understand it will not be shown again.
              </span>
            </label>
            <div style={{ display: "flex", gap: 8 }}>
              <button
                type="button"
                onClick={copy}
                style={{
                  flex: 1,
                  background: copied ? "#16a34a" : "#2563eb",
                  color: "#fff",
                  border: "none",
                  borderRadius: 4,
                  padding: "10px 0",
                  fontSize: 13,
                  cursor: "pointer",
                }}
              >
                {copied ? "Copied" : "Copy to clipboard"}
              </button>
              <button
                type="button"
                disabled={!savedAck}
                onClick={() => {
                  if (!savedAck) return;
                  onActivated(issuedKey);
                }}
                title={savedAck
                  ? "Sign in with your new key"
                  : "Tick the checkbox above to confirm you've saved the key"}
                style={{
                  flex: 1,
                  background: savedAck ? "#16a34a" : "#7f1d1d",
                  color: "#fff",
                  border: savedAck
                    ? "1px solid #22c55e"
                    : "1px solid rgba(239,68,68,0.8)",
                  borderRadius: 4,
                  padding: "10px 0",
                  fontSize: 13,
                  fontWeight: 500,
                  cursor: savedAck ? "pointer" : "not-allowed",
                  opacity: savedAck ? 1 : 0.75,
                  transition: "background 0.15s, border-color 0.15s, opacity 0.15s",
                }}
              >
                Sign in with this key →
              </button>
            </div>
          </>
        ) : (
          <form
            onSubmit={handleSubmit}
            style={{ display: "flex", flexDirection: "column", gap: 12 }}
          >
            <div style={{ fontSize: 13, color: "rgba(255,255,255,0.55)", lineHeight: 1.5 }}>
              Paste the <code>enroll_...</code> token your admin shared with you —
              or, on a fresh j9t install, the one printed to <code>log/log.txt</code> at
              startup. The token is single-use and expires within 30 minutes of creation
              (60 minutes for the first-run bootstrap token).
            </div>
            <input
              type="text"
              placeholder="enroll_..."
              value={token}
              onChange={(e) => setTokenInput(e.target.value)}
              autoFocus
              style={{
                boxSizing: "border-box",
                background: "#2a2a2a",
                border: "1px solid rgba(255,255,255,0.15)",
                borderRadius: 4,
                padding: "10px 12px",
                color: "#e8eef5",
                fontSize: 13,
                fontFamily: "monospace",
                outline: "none",
              }}
            />
            {error && <div style={{ color: "#ff6b6b", fontSize: 13 }}>{error}</div>}
            <div style={{ display: "flex", gap: 8 }}>
              <button
                type="submit"
                disabled={submitting || !token.trim()}
                style={{
                  flex: 1,
                  background: submitting ? "#333" : "#2563eb",
                  color: "#fff",
                  border: "none",
                  borderRadius: 4,
                  padding: "10px 0",
                  fontSize: 14,
                  fontWeight: 500,
                  cursor: submitting ? "wait" : "pointer",
                  opacity: !token.trim() ? 0.5 : 1,
                }}
              >
                {submitting ? "Activating..." : "Activate"}
              </button>
              <button
                type="button"
                onClick={onClose}
                disabled={submitting}
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
