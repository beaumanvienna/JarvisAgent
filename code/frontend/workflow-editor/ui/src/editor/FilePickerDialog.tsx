import React, { useEffect, useState } from "react";
import { listWorkflowFiles, type WorkflowFileEntry } from "../api/workflows";

type Props = {
  workflowId: string;
  canvasFileName: string; // "<id>.json" — the canvas, hidden from the picker
  existingFileNodeRelPaths: Set<string>;
  onPick: (relPath: string) => void;
  onUploadNew: () => void;
  onClose: () => void;
};

// Hide the JCWF machinery and runtime outputs — the picker is for INPUT artifacts (CSVs, text, etc.).
function isInputArtifact(path: string, canvasFileName: string): boolean
{
  if (path === "global.json" || path === canvasFileName) return false;
  if (path === ".history" || path.startsWith(".history/")) return false;
  if (/\.output\.(txt|json)$/.test(path)) return false;
  if (/(^|\/)(stdout|stderr)\.txt$/.test(path)) return false;
  if (/(^|\/)(meta|manifest)\.json$/.test(path)) return false;
  if (/\.transcript\.json$/.test(path)) return false;
  return true;
}

// Deferred-1 — "+ file" picker. Lets the user add an already-uploaded workflow-folder file as a node
// without re-uploading, alongside the upload path. Reads the folder via listWorkflowFiles.
export default function FilePickerDialog(props: Props): React.ReactElement
{
  const { workflowId, canvasFileName, existingFileNodeRelPaths, onPick, onUploadNew, onClose } = props;
  const [files, setFiles] = useState<WorkflowFileEntry[] | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    listWorkflowFiles(workflowId)
      .then((all) => { if (!cancelled) setFiles(all.filter((f) => isInputArtifact(f.path, canvasFileName))); })
      .catch((e) => { if (!cancelled) setError(e instanceof Error ? e.message : String(e)); });
    return () => { cancelled = true; };
  }, [workflowId, canvasFileName]);

  return (
    <div className="modalOverlay" onClick={onClose}>
      <div className="modalContent" style={{ minWidth: 420, maxWidth: 560 }} onClick={(e) => e.stopPropagation()}>
        <div className="modalHeader">
          <span>Add a file</span>
          <button className="btn" type="button" onClick={onClose} style={{ padding: "2px 8px" }}>✕</button>
        </div>

        <div className="modalBody" style={{ display: "flex", flexDirection: "column", gap: 10 }}>
          <button
            className="btn"
            type="button"
            style={{ alignSelf: "flex-start" }}
            onClick={() => { onUploadNew(); onClose(); }}
          >
            {"\u{1F4C4}"} Upload a new file…
          </button>

          <div className="small" style={{ opacity: 0.7 }}>…or pick a file already in this workflow:</div>

          {error && <div className="small" style={{ color: "#ff8a8a" }}>Could not list files: {error}</div>}
          {!files && !error && <div className="small" style={{ opacity: 0.6 }}>Loading…</div>}
          {files && files.length === 0 && (
            <div className="small" style={{ opacity: 0.6 }}>No input files yet — upload one above.</div>
          )}

          {files && files.length > 0 && (
            <div style={{ maxHeight: 280, overflow: "auto", display: "flex", flexDirection: "column", gap: 2 }}>
              {files.map((f) => {
                const already = existingFileNodeRelPaths.has(f.path);
                return (
                  <button
                    key={f.path}
                    type="button"
                    className="btn"
                    disabled={already}
                    title={already ? "Already on the canvas" : `Add ${f.path}`}
                    style={{ display: "flex", justifyContent: "space-between", gap: 8, textAlign: "left", opacity: already ? 0.5 : 1 }}
                    onClick={() => { onPick(f.path); onClose(); }}
                  >
                    <span style={{ overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>
                      {"\u{1F4C4}"} {f.path}{already ? " (on canvas)" : ""}
                    </span>
                    <span className="small" style={{ opacity: 0.6, flexShrink: 0 }}>{f.size_bytes} B</span>
                  </button>
                );
              })}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
