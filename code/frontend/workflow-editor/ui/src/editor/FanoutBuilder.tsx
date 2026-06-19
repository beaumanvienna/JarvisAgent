import React, { useState } from "react";
import type { JcwfFilter } from "../jcwf/types";
import { buildFanoutBinding, previewFanoutBinding, type ProbBinding } from "./buildFanoutBinding";
import { getWorkflowFile } from "../api/workflows";

type Props = {
  filter: JcwfFilter;
  workflowId: string | null;
  onApply: (entry: ProbBinding) => void;
};

// Phase 3 / area J — the no-code fan-out builder. For a per_item ai_call wired to a CSV filter, the
// user ticks the columns to feed each row's prompt; the editor parses the CSV header, shows a live
// row-1 preview, and emits the inline prob_files entry (unique-per-row filename + {{binding.field}}
// content) via buildFanoutBinding. The user never types a template, never learns the filename-
// uniqueness rule, never sees the inline/ref distinction.
export default function FanoutBuilder(props: Props): React.ReactElement
{
  const { filter, workflowId, onApply } = props;
  const binding = (filter.binding && filter.binding.trim().length > 0) ? filter.binding : (filter.id || "item");
  const source = filter.source;
  const declaredColumns = (Array.isArray(source.columns) ? source.columns : Array.isArray(source.fields) ? source.fields : [])
    .filter((c) => typeof c === "string" && c.trim().length > 0);

  const [columns, setColumns] = useState<string[]>(declaredColumns);
  const [firstRow, setFirstRow] = useState<Record<string, string>>({});
  const [selected, setSelected] = useState<string[]>(declaredColumns);
  const [loaded, setLoaded] = useState<boolean>(declaredColumns.length > 0);
  const [error, setError] = useState<string | null>(null);

  const delimiter = typeof source.delimiter === "string" && source.delimiter.length > 0 ? source.delimiter : ",";

  // Parse the CSV header (and first data row for the preview) from the filter's source file.
  const loadColumns = async () => {
    setError(null);
    if (source.kind !== "csv" || !source.path)
    {
      setError("Fan-out builder supports CSV filters with a source file.");
      return;
    }
    if (!workflowId)
    {
      setError("Save the workflow first, then load columns.");
      return;
    }
    try
    {
      const text = await getWorkflowFile(workflowId, source.path);
      const lines = text.split(/\r?\n/).filter((l) => l.length > 0);
      if (lines.length === 0)
      {
        setError("Source CSV is empty.");
        return;
      }
      const header = lines[0].split(delimiter).map((c) => c.trim());
      const row1 = lines.length > 1 ? lines[1].split(delimiter).map((c) => c.trim()) : [];
      const rowMap: Record<string, string> = {};
      header.forEach((c, i) => { rowMap[c] = row1[i] ?? ""; });
      setColumns(header);
      setFirstRow(rowMap);
      setSelected(header);
      setLoaded(true);
    }
    catch (e)
    {
      setError(`Could not read ${source.path}: ${e instanceof Error ? e.message : String(e)}`);
    }
  };

  const toggle = (col: string) => {
    setSelected((prev) => prev.includes(col) ? prev.filter((c) => c !== col) : [...prev, col]);
  };

  // Preserve the header order in the emitted binding regardless of click order.
  const orderedSelected = columns.filter((c) => selected.includes(c));
  const generated = buildFanoutBinding(binding, orderedSelected);
  const preview = previewFanoutBinding(generated, binding, firstRow);

  return (
    <details className="field" style={{ borderLeft: "2px solid rgba(180,140,255,0.4)", paddingLeft: 8 }}>
      <summary style={{ cursor: "pointer", fontSize: 12, color: "rgba(200,170,255,0.95)" }}>
        Fan-out builder (per row → one AI call)
      </summary>
      <div className="small" style={{ marginTop: 6, opacity: 0.8 }}>
        Tick the columns to include in each row&apos;s prompt. One AI call runs per row of
        <code> {source.path || filter.id}</code>; the builder names each call&apos;s file uniquely for you.
      </div>

      {!loaded && (
        <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11, marginTop: 6 }} onClick={() => { void loadColumns(); }}>
          Load columns from {source.path || "CSV"}
        </button>
      )}
      {error && <div className="small" style={{ color: "#ff8a8a", marginTop: 4 }}>{error}</div>}

      {columns.length > 0 && (
        <>
          <div style={{ marginTop: 6, display: "flex", flexWrap: "wrap", gap: 8 }}>
            {columns.map((col) => (
              <label key={col} className="small" style={{ display: "inline-flex", alignItems: "center", gap: 4 }}>
                <input type="checkbox" checked={selected.includes(col)} onChange={() => { toggle(col); }} />
                {col}
              </label>
            ))}
          </div>

          <div className="small" style={{ fontWeight: 600, marginTop: 8 }}>Row 1 preview</div>
          <div className="small" style={{ opacity: 0.7 }}>file: <code>{preview.path}</code></div>
          <pre style={{ fontSize: 11, marginTop: 2, padding: 6, background: "rgba(255,255,255,0.04)", borderRadius: 3, whiteSpace: "pre-wrap" }}>{preview.content || "(no columns selected)"}</pre>

          <button
            className="btn"
            type="button"
            style={{ padding: "3px 8px", fontSize: 11, marginTop: 4 }}
            disabled={orderedSelected.length === 0}
            onClick={() => { onApply(generated); }}
          >
            Apply fan-out prompt
          </button>
        </>
      )}
    </details>
  );
}
