import React, { useCallback, useState } from "react";
import { unlockKeys } from "../api/keys";

type MasterPasswordDialogProps = {
  reason: "no_password" | "wrong_password";
  onUnlocked: (password: string) => void;
};

export default function MasterPasswordDialog({
  reason,
  onUnlocked,
}: MasterPasswordDialogProps): JSX.Element
{
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
      if (!password.trim())
      {
        setError("Password cannot be empty.");
        return;
      }

      setSubmitting(true);
      setError(null);

      try
      {
        const result = await unlockKeys(password);
        if (result.ok)
        {
          onUnlocked(password);
        }
        else
        {
          setError(result.message ?? "Incorrect password. Please try again.");
        }
      }
      catch (err)
      {
        setError("Failed to connect to server.");
      }
      finally
      {
        setSubmitting(false);
      }
    },
    [password, onUnlocked],
  );

  return (
    <div className="modalOverlay">
      <div className="modalContent" style={{ maxWidth: 420 }}>
        <div className="modalHeader">
          <h2 style={{ margin: 0, fontSize: 16 }}>Master Password Required</h2>
        </div>

        <form onSubmit={handleSubmit}>
          <div className="modalBody">
            <p style={{ marginTop: 0, marginBottom: 16, color: "#ccc" }}>
              {heading}. Please enter your master password to unlock the encrypted keys file.
            </p>

            <div style={{ position: "relative" }}>
              <input
                type={showPassword ? "text" : "password"}
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                placeholder="Master password"
                autoFocus
                disabled={submitting}
                style={{
                  width: "100%",
                  padding: "8px 36px 8px 12px",
                  fontSize: 14,
                  borderRadius: 4,
                  border: "1px solid #555",
                  background: "#1e1e1e",
                  color: "#eee",
                  boxSizing: "border-box",
                }}
              />
              <button
                type="button"
                onClick={() => setShowPassword((prev) => !prev)}
                style={{
                  position: "absolute",
                  right: 8,
                  top: "50%",
                  transform: "translateY(-50%)",
                  background: "none",
                  border: "none",
                  cursor: "pointer",
                  color: "#aaa",
                  fontSize: 16,
                  padding: 0,
                  lineHeight: 1,
                }}
                title={showPassword ? "Hide password" : "Show password"}
              >
                {showPassword ? "\u{1F441}" : "\u{1F441}\u{200D}\u{1F5E8}"}
              </button>
            </div>

            {error && (
              <div
                style={{
                  marginTop: 8,
                  padding: "6px 10px",
                  borderRadius: 4,
                  background: "#3a1c1c",
                  color: "#f88",
                  fontSize: 13,
                }}
              >
                {error}
              </div>
            )}
          </div>

          <div className="modalFooter">
            <button className="btn" type="submit" disabled={submitting}>
              {submitting ? "Unlocking..." : "Unlock"}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
