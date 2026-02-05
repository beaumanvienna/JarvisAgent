import React from "react";
import type { EditorSettings } from "../App";

type SettingsModalProps = {
  settings: EditorSettings;
  onSettingsChange: (settings: EditorSettings) => void;
  onClose: () => void;
};

export default function SettingsModal({
  settings,
  onSettingsChange,
  onClose,
}: SettingsModalProps): JSX.Element
{
  const handleCheckboxChange = (key: keyof EditorSettings) => (
    e: React.ChangeEvent<HTMLInputElement>
  ) => {
    onSettingsChange({ ...settings, [key]: e.target.checked });
  };

  return (
    <div className="modalOverlay" onClick={onClose}>
      <div className="modalContent" onClick={(e) => e.stopPropagation()}>
        <div className="modalHeader">
          <h2 style={{ margin: 0 }}>Settings</h2>
          <button className="btn" type="button" onClick={onClose}>×</button>
        </div>

        <div className="modalBody">
          <div style={{ marginBottom: 16 }}>
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
          </div>
        </div>

        <div className="modalFooter">
          <button className="btn" type="button" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>
  );
}
