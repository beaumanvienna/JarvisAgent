import { useCallback, useState } from "react";
import { unlockKeys } from "../api";

type MasterPasswordDialogProps = {
  reason: "no_password" | "wrong_password";
  onUnlocked: () => void;
};

export default function MasterPasswordDialog({
  reason,
  onUnlocked,
}: MasterPasswordDialogProps) {
  const [password, setPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);
  const [showPassword, setShowPassword] = useState(false);

  const heading =
    reason === "no_password"
      ? "No master password provided"
      : "Incorrect master password provided";

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
        if (result.ok) {
          onUnlocked();
        } else {
          setError(result.message ?? "Incorrect password. Please try again.");
        }
      } catch {
        setError("Failed to connect to server.");
      } finally {
        setSubmitting(false);
      }
    },
    [password, onUnlocked],
  );

  return (
    <div className="mpd-overlay">
      <div className="mpd-dialog">
        <div className="mpd-header">
          <h2>Master Password Required</h2>
        </div>

        <form onSubmit={handleSubmit}>
          <div className="mpd-body">
            <p className="mpd-description">
              {heading}. Please enter your master password to unlock the
              encrypted keys file.
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

          <div className="mpd-footer">
            <button className="btn btn-editor" type="submit" disabled={submitting}>
              {submitting ? "Unlocking..." : "Unlock"}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
