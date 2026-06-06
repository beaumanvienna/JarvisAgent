import { useCallback, useState } from "react";
import { unlockKeys, type IssuedAdminKey } from "@shared/api/keys";

// Two purposes, one dialog:
//  - "unlock"  (default): first-run bootstrap / unlock of the encrypted key
//    stores via POST /api/settings/keys/unlock.  May surface a freshly-minted
//    admin MCP key on a second screen.
//  - "confirm": second-factor re-auth for a routing/connection mutation.  Does
//    NOT call the server itself — it resolves the entered password to the caller
//    via onConfirm(), which performs the mutation (attaching master_password) and
//    reports back {ok, message}.  Closes the "unattended unlocked dashboard"
//    vector: every AI-interface / connection change re-prompts for the password.
type UnlockProps = {
  mode?: "unlock";
  reason: "no_password" | "wrong_password" | "no_keys_file";
  onUnlocked: () => void;
};

type ConfirmProps = {
  mode: "confirm";
  title?: string;
  description?: string;
  confirmLabel?: string;
  onConfirm: (password: string) => Promise<{ ok: boolean; message?: string }>;
  onCancel: () => void;
};

type MasterPasswordDialogProps = UnlockProps | ConfirmProps;

export default function MasterPasswordDialog(props: MasterPasswordDialogProps) {
  if (props.mode === "confirm") {
    return <ConfirmDialog {...props} />;
  }
  return <UnlockDialog {...props} />;
}

// ───────────────────────────────────────────────────────────────────────────
// Re-auth confirm dialog
// ───────────────────────────────────────────────────────────────────────────
function ConfirmDialog({ title, description, confirmLabel, onConfirm, onCancel }: ConfirmProps) {
  const [password, setPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);
  const [showPassword, setShowPassword] = useState(false);

  const handleSubmit = useCallback(
    async (e: React.FormEvent) => {
      e.preventDefault();
      if (!password.trim()) {
        setError("Password cannot be empty.");
        return;
      }
      setSubmitting(true);
      setError(null);
      try {
        const result = await onConfirm(password);
        if (!result.ok) {
          setError(result.message ?? "Incorrect master password. Please try again.");
          return;
        }
        // Success — the caller unmounts this dialog (it owns visibility).
      } catch {
        setError("Failed to apply change.");
      } finally {
        setSubmitting(false);
      }
    },
    [password, onConfirm],
  );

  return (
    <div className="mpd-overlay">
      <div className="mpd-dialog">
        <div className="mpd-header">
          <h2>{title ?? "Confirm master password"}</h2>
        </div>
        <form onSubmit={handleSubmit}>
          <div className="mpd-body">
            <p className="mpd-description">
              {description ?? "Re-enter your master password to apply this change."}
            </p>
            <div className="mpd-input-wrap">
              <input
                type={showPassword ? "text" : "password"}
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                placeholder="Master password"
                autoFocus
                disabled={submitting}
                className="mpd-input"
              />
              <button
                type="button"
                onClick={() => setShowPassword((prev) => !prev)}
                className="mpd-eye-btn"
                title={showPassword ? "Hide password" : "Show password"}
              >
                {showPassword ? "\u{1F441}" : "\u{1F441}\u{200D}\u{1F5E8}"}
              </button>
            </div>
            {error && <div className="mpd-error">{error}</div>}
          </div>
          <div className="mpd-footer" style={{ display: "flex", gap: 8 }}>
            <button
              type="button"
              className="btn"
              onClick={onCancel}
              disabled={submitting}
              style={{ flex: 1 }}
            >
              Cancel
            </button>
            <button className="btn btn-editor" type="submit" disabled={submitting} style={{ flex: 1 }}>
              {submitting ? "Applying..." : confirmLabel ?? "Confirm"}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}

// ───────────────────────────────────────────────────────────────────────────
// Unlock / bootstrap dialog (unchanged behaviour)
// ───────────────────────────────────────────────────────────────────────────
function UnlockDialog({ reason, onUnlocked }: UnlockProps) {
  const [password, setPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);
  const [showPassword, setShowPassword] = useState(false);
  const [issuedKey, setIssuedKey] = useState<IssuedAdminKey | null>(null);
  const [copied, setCopied] = useState(false);

  const isBootstrap = reason === "no_keys_file";
  const heading = isBootstrap
    ? "Set a new master password"
    : reason === "no_password"
    ? "No master password provided"
    : "Incorrect master password provided";
  const description = isBootstrap
    ? "Welcome — this j9t instance has no encrypted key store yet. Choose a master password now; it will protect your AI provider credentials (keys.json.enc) and MCP API keys (mcp_keys.json.enc). Save it somewhere safe — j9t never stores it on disk."
    : `${heading}. Please enter your master password to unlock the encrypted keys file.`;
  const title = isBootstrap ? "Set Master Password" : "Master Password Required";
  const submitLabel = isBootstrap
    ? submitting ? "Creating..." : "Set password"
    : submitting ? "Unlocking..." : "Unlock";

  const handleSubmit = useCallback(
    async (e: React.FormEvent) => {
      e.preventDefault();
      if (!password.trim()) {
        setError("Password cannot be empty.");
        return;
      }

      setSubmitting(true);
      setError(null);

      try {
        const result = await unlockKeys(password);
        if (!result.ok) {
          setError(result.message ?? "Incorrect password. Please try again.");
          return;
        }
        // The server hands back a freshly-minted admin MCP key whenever the
        // key store was empty after unlock (bootstrap or missing mcp_keys).
        // Intercept here so the user sees it in the UI — the admin key is
        // shown exactly once, same as the activation flow.
        if (result.admin_key) {
          setIssuedKey(result.admin_key);
          return;
        }
        onUnlocked();
      } catch {
        setError("Failed to connect to server.");
      } finally {
        setSubmitting(false);
      }
    },
    [password, onUnlocked],
  );

  const copy = async () => {
    if (!issuedKey) return;
    try {
      await navigator.clipboard.writeText(issuedKey.api_key);
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    } catch {
      setCopied(false);
    }
  };

  // ─────────────────────────────────────────────────────────────────────────
  // Second screen: bootstrap completed, admin key displayed. Shown exactly
  // once — if the user closes this without copying, they'd have to delete
  // mcp_keys.json.enc and restart to get another one.
  // ─────────────────────────────────────────────────────────────────────────
  if (issuedKey) {
    return (
      <div className="mpd-overlay">
        <div className="mpd-dialog">
          <div className="mpd-header">
            <h2>Your admin MCP API key</h2>
          </div>
          <div className="mpd-body">
            <p className="mpd-description">
              Master password set. Here's the MCP API key for{" "}
              <strong>{issuedKey.user}</strong> (role: <code>{issuedKey.role}</code>).
              It is shown <strong>exactly once</strong> — copy it to a password
              manager or your Claude Code config before continuing.
            </p>
            <div
              style={{
                background: "rgba(0,0,0,0.3)",
                border: "1px solid rgba(255,255,255,0.15)",
                borderRadius: 4,
                padding: "10px 12px",
                fontFamily: "monospace",
                fontSize: 12,
                color: "#e8eef5",
                wordBreak: "break-all",
                userSelect: "all",
                marginBottom: 12,
              }}
            >
              {issuedKey.api_key}
            </div>
            <div style={{ fontSize: 11, color: "rgba(255,255,255,0.5)" }}>
              Key ID: <code>{issuedKey.key_id}</code> &middot; Expires:{" "}
              {issuedKey.expires_at}
            </div>
          </div>
          <div className="mpd-footer" style={{ display: "flex", gap: 8 }}>
            <button
              type="button"
              className="btn btn-editor"
              onClick={copy}
              style={{ flex: 1, background: copied ? "#16a34a" : undefined }}
            >
              {copied ? "Copied" : "Copy to clipboard"}
            </button>
            <button
              type="button"
              className="btn"
              onClick={onUnlocked}
              style={{ flex: 1 }}
            >
              Done
            </button>
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="mpd-overlay">
      <div className="mpd-dialog">
        <div className="mpd-header">
          <h2>{title}</h2>
        </div>

        <form onSubmit={handleSubmit}>
          <div className="mpd-body">
            <p className="mpd-description">{description}</p>

            <div className="mpd-input-wrap">
              <input
                type={showPassword ? "text" : "password"}
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                placeholder="Master password"
                autoFocus
                disabled={submitting}
                className="mpd-input"
              />
              <button
                type="button"
                onClick={() => setShowPassword((prev) => !prev)}
                className="mpd-eye-btn"
                title={showPassword ? "Hide password" : "Show password"}
              >
                {showPassword ? "\u{1F441}" : "\u{1F441}\u{200D}\u{1F5E8}"}
              </button>
            </div>

            {error && <div className="mpd-error">{error}</div>}
          </div>

          <div className="mpd-footer">
            <button className="btn btn-editor" type="submit" disabled={submitting}>
              {submitLabel}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
