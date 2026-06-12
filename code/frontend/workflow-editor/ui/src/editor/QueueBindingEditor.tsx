import React, { useCallback, useState } from "react";
import type { JcwfQueueBinding, JcwfQueueFileEntry } from "../jcwf/types";
import TemplateTextarea from "./TemplateTextarea";
import FilePathInput from "./FilePathInput";

type FileCategory = "stng_files" | "task_files" | "cntx_files" | "prob_files";

const CATEGORIES: { key: FileCategory; label: string }[] = [
  { key: "stng_files", label: "STNG (Settings)" },
  { key: "task_files", label: "TASK (Task)" },
  { key: "cntx_files", label: "CNTX (Context)" },
  { key: "prob_files", label: "PROB (Problem)" },
];

function isInlineEntry(entry: JcwfQueueFileEntry | string): entry is JcwfQueueFileEntry
{
  return typeof entry === "object" && entry !== null && "path" in entry;
}

function entryPath(entry: JcwfQueueFileEntry | string): string
{
  return isInlineEntry(entry) ? entry.path : entry;
}

function entryContent(entry: JcwfQueueFileEntry | string): string
{
  return isInlineEntry(entry) ? entry.content : "";
}

type Props = {
  queueBinding: JcwfQueueBinding | undefined;
  onChange: (binding: JcwfQueueBinding) => void;
  templateVariables?: string[];
};

export default function QueueBindingEditor({ queueBinding, onChange, templateVariables }: Props): JSX.Element
{
  const binding: JcwfQueueBinding = queueBinding ?? {};
  const [expandedEntries, setExpandedEntries] = useState<Record<string, boolean>>({});

  const toggleExpanded = useCallback((entryKey: string) => {
    setExpandedEntries((prev) => ({ ...prev, [entryKey]: !prev[entryKey] }));
  }, []);

  const updateCategory = useCallback((category: FileCategory, entries: (JcwfQueueFileEntry | string)[]) => {
    const next: JcwfQueueBinding = { ...binding };
    if (entries.length > 0)
    {
      next[category] = entries;
    }
    else
    {
      delete next[category];
    }
    onChange(next);
  }, [binding, onChange]);

  const addEntry = useCallback((category: FileCategory) => {
    const existing = binding[category] ?? [];
    const prefix = category.replace("_files", "").toUpperCase();
    const newEntry: JcwfQueueFileEntry = {
      path: `${prefix}_new_${existing.length + 1}.txt`,
      content: "",
    };
    updateCategory(category, [...existing, newEntry]);
  }, [binding, updateCategory]);

  const removeEntry = useCallback((category: FileCategory, index: number) => {
    const existing = binding[category] ?? [];
    const next = existing.filter((_, i) => i !== index);
    updateCategory(category, next);
  }, [binding, updateCategory]);

  const updateEntryPath = useCallback((category: FileCategory, index: number, newPath: string) => {
    const existing = [...(binding[category] ?? [])];
    const entry = existing[index];
    if (isInlineEntry(entry))
    {
      existing[index] = { ...entry, path: newPath };
    }
    else
    {
      existing[index] = newPath;
    }
    updateCategory(category, existing);
  }, [binding, updateCategory]);

  const updateEntryContent = useCallback((category: FileCategory, index: number, newContent: string) => {
    const existing = [...(binding[category] ?? [])];
    const entry = existing[index];
    if (isInlineEntry(entry))
    {
      existing[index] = { ...entry, content: newContent };
    }
    else
    {
      existing[index] = { path: entry, content: newContent };
    }
    updateCategory(category, existing);
  }, [binding, updateCategory]);

  const toggleInline = useCallback((category: FileCategory, index: number) => {
    const existing = [...(binding[category] ?? [])];
    const entry = existing[index];
    if (isInlineEntry(entry))
    {
      existing[index] = entry.path;
    }
    else
    {
      existing[index] = { path: entry, content: "" };
    }
    updateCategory(category, existing);
  }, [binding, updateCategory]);

  const moveEntry = useCallback((category: FileCategory, index: number, direction: -1 | 1) => {
    const existing = [...(binding[category] ?? [])];
    const targetIndex = index + direction;
    if (targetIndex < 0 || targetIndex >= existing.length) return;
    [existing[index], existing[targetIndex]] = [existing[targetIndex], existing[index]];
    updateCategory(category, existing);
  }, [binding, updateCategory]);

  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
      <div style={{ fontWeight: 700, fontSize: 12 }}>queue_binding</div>

      {CATEGORIES.map(({ key, label }) => {
        const entries = binding[key] ?? [];
        return (
          <div key={key} className="qbCategory">
            <div className="qbCategoryHeader">
              <span className="small" style={{ fontWeight: 600 }}>{label}</span>
              <span className="qbBadge">{entries.length}</span>
              <button
                className="btn"
                type="button"
                style={{ padding: "2px 8px", fontSize: 11, marginLeft: "auto" }}
                onClick={() => addEntry(key)}
              >+</button>
            </div>

            {entries.map((entry, idx) => {
              const entryKey = `${key}-${idx}`;
              const path = entryPath(entry);
              const content = entryContent(entry);
              const inline = isInlineEntry(entry);
              const expanded = expandedEntries[entryKey] ?? inline;

              return (
                <div key={entryKey} className="qbEntry">
                  <div style={{ display: "flex", gap: 4, alignItems: "center" }}>
                    <FilePathInput
                      className="input"
                      style={{ fontSize: 12, padding: "4px 8px", flex: 1 }}
                      value={path}
                      onChange={(v) => updateEntryPath(key, idx, v)}
                      placeholder="file path"
                    />
                    <button
                      className="btn"
                      type="button"
                      style={{ padding: "2px 6px", fontSize: 10 }}
                      title="Move up"
                      disabled={idx === 0}
                      onClick={() => moveEntry(key, idx, -1)}
                    >▲</button>
                    <button
                      className="btn"
                      type="button"
                      style={{ padding: "2px 6px", fontSize: 10 }}
                      title="Move down"
                      disabled={idx === entries.length - 1}
                      onClick={() => moveEntry(key, idx, 1)}
                    >▼</button>
                    <div style={{ display: "inline-flex", flexShrink: 0, borderRadius: 4, overflow: "hidden", border: "1px solid rgba(255,255,255,0.18)" }}>
                      <button
                        type="button"
                        title="Inline: this content is written into the queue folder under this filename"
                        onClick={() => { if (!inline) toggleInline(key, idx); }}
                        style={{
                          padding: "2px 6px", fontSize: 10, border: "none", cursor: inline ? "default" : "pointer",
                          background: inline ? "rgba(100,180,255,0.40)" : "transparent",
                          color: inline ? "#fff" : "rgba(255,255,255,0.55)",
                        }}
                      >inline</button>
                      <button
                        type="button"
                        title="Ref: a path to an existing file the runtime reads"
                        onClick={() => { if (inline) toggleInline(key, idx); }}
                        style={{
                          padding: "2px 6px", fontSize: 10, border: "none", cursor: !inline ? "default" : "pointer",
                          background: !inline ? "rgba(100,180,255,0.40)" : "transparent",
                          color: !inline ? "#fff" : "rgba(255,255,255,0.55)",
                        }}
                      >ref</button>
                    </div>
                    <button
                      className="btn"
                      type="button"
                      style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }}
                      onClick={() => removeEntry(key, idx)}
                    >x</button>
                  </div>

                  {inline && (
                    <div>
                      <button
                        type="button"
                        className="btnLink"
                        style={{ fontSize: 11, opacity: 0.7, cursor: "pointer", background: "none", border: "none", color: "inherit", padding: 0 }}
                        onClick={() => toggleExpanded(entryKey)}
                      >{expanded ? "collapse content" : `expand content (${content.length} chars)`}</button>

                      {expanded && (
                        <TemplateTextarea
                          className="input codeInput"
                          style={{ fontSize: 11, padding: "6px 8px", marginTop: 4 }}
                          value={content}
                          onChange={(val) => updateEntryContent(key, idx, val)}
                          rows={Math.min(Math.max(content.split("\n").length, 3), 12)}
                          placeholder="inline file content — type {{ for variable autocomplete"
                          templateVariables={templateVariables ?? []}
                        />
                      )}
                    </div>
                  )}
                </div>
              );
            })}
          </div>
        );
      })}
    </div>
  );
}
