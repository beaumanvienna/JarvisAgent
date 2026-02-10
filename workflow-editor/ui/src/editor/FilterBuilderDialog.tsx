import React, { useState } from "react";
import type { JcwfFilter, JcwfFilterSource, JcwfFilterSourceKind } from "../jcwf/types";

type Props = {
  isOpen: boolean;
  filter: JcwfFilter;
  onSave: (filter: JcwfFilter) => void;
  onCancel: () => void;
};

function defaultSource(kind: JcwfFilterSourceKind): JcwfFilterSource
{
  switch (kind)
  {
    case "csv":
      return { kind: "csv", path: "", delimiter: ",", header_row: 1 };
    case "text_lines":
      return { kind: "text_lines", path: "" };
    case "query":
      return { kind: "query", query: "", index_path: "" };
    case "polarion_query":
      return { kind: "polarion_query", query: "", base_url: "", project_id: "", key_name: "" };
  }
}

export default function FilterBuilderDialog({ isOpen, filter, onSave, onCancel }: Props): JSX.Element | null
{
  const [draft, setDraft] = useState<JcwfFilter>(() => structuredClone(filter));

  if (!isOpen)
  {
    return null;
  }

  const source = draft.source;

  const updateSource = (patch: Partial<JcwfFilterSource>) =>
  {
    setDraft((prev) => ({ ...prev, source: { ...prev.source, ...patch } }));
  };

  const changeKind = (kind: JcwfFilterSourceKind) =>
  {
    if (kind === source.kind)
    {
      return;
    }
    setDraft((prev) => ({ ...prev, source: defaultSource(kind) }));
  };

  return (
    <div className="modalOverlay" onClick={onCancel}>
      <div className="modalContent" style={{ minWidth: 520, maxWidth: 640 }} onClick={(e) => e.stopPropagation()}>
        <div className="modalHeader">
          <span>Filter: {draft.id}</span>
          <button className="btn" type="button" onClick={onCancel} style={{ padding: "2px 8px" }}>✕</button>
        </div>

        <div className="modalBody" style={{ display: "flex", flexDirection: "column", gap: 12 }}>

          <label className="field">
            <div className="small">Filter ID</div>
            <input
              className="input"
              value={draft.id}
              onChange={(e) => setDraft((prev) => ({ ...prev, id: e.target.value }))}
            />
          </label>

          <label className="field">
            <div className="small">Binding prefix</div>
            <input
              className="input"
              value={draft.binding ?? "item"}
              onChange={(e) => setDraft((prev) => ({ ...prev, binding: e.target.value }))}
              placeholder="item"
            />
          </label>

          <label className="field">
            <div className="small">Max items (0 = unlimited)</div>
            <input
              className="input"
              type="number"
              value={draft.max_items ?? 10000}
              onChange={(e) => setDraft((prev) => ({ ...prev, max_items: parseInt(e.target.value) || 0 }))}
            />
          </label>

          <hr style={{ border: "none", borderTop: "1px solid rgba(255,255,255,0.08)", margin: "4px 0" }} />

          <label className="field">
            <div className="small" style={{ fontWeight: 600 }}>Source kind</div>
            <select
              className="input"
              value={source.kind}
              onChange={(e) => changeKind(e.target.value as JcwfFilterSourceKind)}
            >
              <option value="csv">csv</option>
              <option value="text_lines">text_lines</option>
              <option value="query">query</option>
              <option value="polarion_query">polarion_query</option>
            </select>
          </label>

          {(source.kind === "csv" || source.kind === "text_lines") && (
            <label className="field">
              <div className="small">File path</div>
              <input
                className="input"
                value={source.path ?? ""}
                onChange={(e) => updateSource({ path: e.target.value })}
                placeholder="relative to workflow base dir"
              />
            </label>
          )}

          {source.kind === "csv" && (
            <>
              <label className="field">
                <div className="small">Delimiter</div>
                <input
                  className="input"
                  value={source.delimiter ?? ","}
                  onChange={(e) => updateSource({ delimiter: e.target.value })}
                  style={{ maxWidth: 60 }}
                />
              </label>
              <label className="field">
                <div className="small">Header row (0 = no header)</div>
                <input
                  className="input"
                  type="number"
                  value={source.header_row ?? 1}
                  onChange={(e) => updateSource({ header_row: parseInt(e.target.value) || 0 })}
                  style={{ maxWidth: 80 }}
                />
              </label>
              <label className="field">
                <div className="small">Columns (comma-separated, empty = all)</div>
                <input
                  className="input"
                  value={(source.columns ?? []).join(", ")}
                  onChange={(e) => updateSource({ columns: e.target.value.split(",").map((s) => s.trim()).filter(Boolean) })}
                  placeholder="col1, col2, ..."
                />
              </label>
              <label className="field">
                <div className="small">Row range (e.g. 1-100, 5-, -50)</div>
                <input
                  className="input"
                  value={source.range ?? ""}
                  onChange={(e) => updateSource({ range: e.target.value })}
                  placeholder="(all rows)"
                />
              </label>
            </>
          )}

          {source.kind === "query" && (
            <>
              <label className="field">
                <div className="small">Query expression</div>
                <textarea
                  className="input codeInput"
                  value={source.query ?? ""}
                  onChange={(e) => updateSource({ query: e.target.value })}
                  rows={3}
                  placeholder='status:"open" AND priority:[1 TO 3]'
                />
              </label>
              <label className="field">
                <div className="small">Index file path</div>
                <input
                  className="input"
                  value={source.index_path ?? ""}
                  onChange={(e) => updateSource({ index_path: e.target.value })}
                  placeholder="relative to workflow base dir"
                />
              </label>
              <label className="field">
                <div className="small">Fields (comma-separated, empty = all)</div>
                <input
                  className="input"
                  value={(source.fields ?? []).join(", ")}
                  onChange={(e) => updateSource({ fields: e.target.value.split(",").map((s) => s.trim()).filter(Boolean) })}
                />
              </label>
            </>
          )}

          {source.kind === "polarion_query" && (
            <>
              <label className="field">
                <div className="small">Base URL</div>
                <input
                  className="input"
                  value={source.base_url ?? ""}
                  onChange={(e) => updateSource({ base_url: e.target.value })}
                  placeholder="https://polarion.example.com/polarion"
                />
              </label>
              <label className="field">
                <div className="small">Project ID</div>
                <input
                  className="input"
                  value={source.project_id ?? ""}
                  onChange={(e) => updateSource({ project_id: e.target.value })}
                />
              </label>
              <label className="field">
                <div className="small">Query expression</div>
                <textarea
                  className="input codeInput"
                  value={source.query ?? ""}
                  onChange={(e) => updateSource({ query: e.target.value })}
                  rows={3}
                  placeholder='type:requirement AND status:approved'
                />
              </label>
              <label className="field">
                <div className="small">Key name (KeyManager credential)</div>
                <input
                  className="input"
                  value={source.key_name ?? ""}
                  onChange={(e) => updateSource({ key_name: e.target.value })}
                />
              </label>
              <label className="field">
                <div className="small">Fields (comma-separated, empty = all)</div>
                <input
                  className="input"
                  value={(source.fields ?? []).join(", ")}
                  onChange={(e) => updateSource({ fields: e.target.value.split(",").map((s) => s.trim()).filter(Boolean) })}
                />
              </label>
              <label className="field">
                <div className="small">Page size</div>
                <input
                  className="input"
                  type="number"
                  value={source.page_size ?? 100}
                  onChange={(e) => updateSource({ page_size: parseInt(e.target.value) || 100 })}
                  style={{ maxWidth: 100 }}
                />
              </label>
            </>
          )}

        </div>

        <div className="modalFooter">
          <button className="btn" type="button" onClick={onCancel}>Cancel</button>
          <button
            className="btn btnPrimary"
            type="button"
            onClick={() => onSave(draft)}
            disabled={!draft.id || !draft.source.kind}
          >
            Save Filter
          </button>
        </div>
      </div>
    </div>
  );
}
