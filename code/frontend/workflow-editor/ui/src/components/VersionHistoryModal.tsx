import React, { useCallback, useEffect, useState } from "react";
import { listVersions, restoreVersion, type VersionEntry } from "../api/versions";

type Props = {
  workflowId: string;
  onClose: () => void;
  onRestored: () => void;
};

function formatTimestamp(ts: string): string
{
  // 20260322T143012 → 2026-03-22 14:30:12
  if (ts.length < 15) return ts;
  const d = ts.slice(0, 4) + "-" + ts.slice(4, 6) + "-" + ts.slice(6, 8);
  const t = ts.slice(9, 11) + ":" + ts.slice(11, 13) + ":" + ts.slice(13, 15);
  return `${d} ${t} UTC`;
}

function formatSize(bytes: number | undefined): string
{
  if (bytes === undefined) return "";
  if (bytes < 1024) return `${bytes} B`;
  return `${(bytes / 1024).toFixed(1)} kB`;
}

export default function VersionHistoryModal({ workflowId, onClose, onRestored }: Props): JSX.Element
{
  const [versions, setVersions] = useState<VersionEntry[]>([]);
  const [loading, setLoading] = useState(true);
  const [restoring, setRestoring] = useState<string | null>(null);
  const [message, setMessage] = useState("");

  const refresh = useCallback(() => {
    setLoading(true);
    listVersions(workflowId)
      .then((res) => {
        setVersions(res.versions);
        setLoading(false);
      })
      .catch(() => {
        setMessage("Failed to load version history");
        setLoading(false);
      });
  }, [workflowId]);

  useEffect(() => { refresh(); }, [refresh]);

  const handleRestore = useCallback(async (ts: string) => {
    setRestoring(ts);
    setMessage("");
    try
    {
      const result = await restoreVersion(workflowId, ts);
      if (result.ok)
      {
        setMessage(`Restored version ${formatTimestamp(ts)}`);
        onRestored();
        refresh();
      }
      else
      {
        setMessage("Restore failed");
      }
    }
    catch
    {
      setMessage("Restore failed (network error)");
    }
    finally
    {
      setRestoring(null);
    }
  }, [workflowId, onRestored, refresh]);

  return (
    <div className="modalOverlay" onClick={onClose}>
      <div className="modalContent" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 520 }}>
        <div className="modalHeader">
          <h2 style={{ margin: 0, fontSize: 16 }}>Version History</h2>
          <button className="btn" type="button" onClick={onClose}>×</button>
        </div>

        <div className="modalBody" style={{ maxHeight: 400, overflowY: "auto" }}>
          <div className="small muted" style={{ marginBottom: 8 }}>
            {workflowId} — {versions.length} saved version{versions.length !== 1 ? "s" : ""}
          </div>

          {loading ? (
            <div className="muted">Loading...</div>
          ) : versions.length === 0 ? (
            <div className="muted">No version history yet. Versions are created automatically on each save.</div>
          ) : (
            <table style={{ width: "100%", borderCollapse: "collapse", fontSize: 12 }}>
              <thead>
                <tr style={{ textAlign: "left", borderBottom: "1px solid rgba(255,255,255,0.1)" }}>
                  <th style={{ padding: "4px 8px", fontWeight: 600 }}>Saved at</th>
                  <th style={{ padding: "4px 8px", fontWeight: 600, width: 70 }}>Size</th>
                  <th style={{ padding: "4px 8px", fontWeight: 600, width: 80 }}></th>
                </tr>
              </thead>
              <tbody>
                {versions.map((v, idx) => (
                  <tr
                    key={v.timestamp}
                    style={{
                      borderBottom: "1px solid rgba(255,255,255,0.05)",
                      background: idx === 0 ? "rgba(120,180,255,0.06)" : undefined,
                    }}
                  >
                    <td style={{ padding: "5px 8px", fontFamily: "monospace", fontSize: 11 }}>
                      {formatTimestamp(v.timestamp)}
                      {idx === 0 && <span style={{ marginLeft: 8, fontSize: 10, opacity: 0.5 }}>latest</span>}
                    </td>
                    <td style={{ padding: "5px 8px", opacity: 0.6 }}>
                      {formatSize(v.sizeBytes)}
                    </td>
                    <td style={{ padding: "5px 8px" }}>
                      <button
                        className="btn"
                        type="button"
                        style={{ fontSize: 11, padding: "2px 8px" }}
                        disabled={restoring !== null}
                        onClick={() => handleRestore(v.timestamp)}
                      >
                        {restoring === v.timestamp ? "Restoring..." : "Restore"}
                      </button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}

          {message && (
            <div style={{
              marginTop: 10,
              padding: "6px 10px",
              borderRadius: 6,
              fontSize: 12,
              background: message.startsWith("Restored") ? "rgba(120,255,170,0.08)" : "rgba(255,120,120,0.08)",
              border: message.startsWith("Restored") ? "1px solid rgba(120,255,170,0.25)" : "1px solid rgba(255,120,120,0.25)",
            }}>
              {message}
            </div>
          )}
        </div>

        <div className="modalFooter">
          <button className="btn" type="button" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>
  );
}
