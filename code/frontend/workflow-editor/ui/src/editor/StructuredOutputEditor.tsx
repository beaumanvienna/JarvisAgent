import React, { useEffect, useState } from "react";
import type { JcwfTask } from "../jcwf/types";
import { inferSchemaFromExample } from "./inferSchemaFromExample";

type Props = {
  task: JcwfTask;
  onChange: (patch: Partial<JcwfTask>) => void;
};

// The field-builder's model of one property. `array`/`object` nest no further here — the raw-JSON
// advanced editor below covers schemas the builder can't express.
type FieldRow = {
  name: string;
  type: string;
  required: boolean;
  enumText: string; // comma-separated; only meaningful for type "string"
};

const FIELD_TYPES = ["string", "number", "integer", "boolean", "object", "array"] as const;

const EXAMPLE_SCHEMA = `{
  "type": "object",
  "properties": {
    "category": { "type": "string", "enum": ["engine", "tires", "rephrase"] }
  },
  "required": ["category"],
  "additionalProperties": false
}`;

function schemaToText(value: unknown): string
{
  return value !== undefined && value !== null ? JSON.stringify(value, null, 2) : "";
}

// Parse an output_schema object into the builder's row model. Anything the builder can't represent
// (nested objects, $ref, etc.) still round-trips through the raw JSON editor — the rows just show
// the top-level shape.
function parseSchemaToRows(schema: unknown): { rows: FieldRow[]; allowExtra: boolean }
{
  if (!schema || typeof schema !== "object" || Array.isArray(schema))
  {
    return { rows: [], allowExtra: false };
  }
  const s = schema as Record<string, unknown>;
  const props = (s.properties && typeof s.properties === "object" && !Array.isArray(s.properties))
    ? (s.properties as Record<string, unknown>) : {};
  const required = Array.isArray(s.required) ? (s.required as unknown[]).map(String) : [];
  const rows: FieldRow[] = Object.entries(props).map(([name, def]) => {
    const d = (def && typeof def === "object" && !Array.isArray(def)) ? (def as Record<string, unknown>) : {};
    return {
      name,
      type: typeof d.type === "string" ? d.type : "string",
      required: required.includes(name),
      enumText: Array.isArray(d.enum) ? (d.enum as unknown[]).map(String).join(", ") : "",
    };
  });
  // additionalProperties defaults to "allowed" in JSON Schema; only an explicit false locks it.
  const allowExtra = s.additionalProperties !== false;
  return { rows, allowExtra };
}

// Build an output_schema object from the builder rows. Rows with an empty name are skipped (a
// half-typed field doesn't corrupt the schema).
function rowsToSchema(rows: FieldRow[], allowExtra: boolean): Record<string, unknown>
{
  const properties: Record<string, unknown> = {};
  const required: string[] = [];
  for (const r of rows)
  {
    const name = r.name.trim();
    if (name.length === 0)
    {
      continue;
    }
    const prop: Record<string, unknown> = { type: r.type };
    if (r.type === "string")
    {
      const values = r.enumText.split(",").map((v) => v.trim()).filter((v) => v.length > 0);
      if (values.length > 0)
      {
        prop.enum = values;
      }
    }
    properties[name] = prop;
    if (r.required)
    {
      required.push(name);
    }
  }
  const schema: Record<string, unknown> = { type: "object", properties };
  if (required.length > 0)
  {
    schema.required = required;
  }
  schema.additionalProperties = allowExtra;
  return schema;
}

// Inspector editor for an ai_call task's structured-output fields (output_schema / output_retries —
// both top-level JCWF task fields). A field builder (name/type/required rows + an "allow extra
// fields" toggle that writes additionalProperties) is the primary UI; the raw JSON Schema textarea
// is an advanced fallback for shapes the builder can't express. Both edit the same output_schema
// object: builder edits regenerate it, raw-text edits re-seed the builder when they parse. A
// validated reply lands at <stem>.output.json.
export default function StructuredOutputEditor(props: Props): React.ReactElement
{
  const { task, onChange } = props;

  const seed = parseSchemaToRows(task.output_schema);
  const [rows, setRows] = useState<FieldRow[]>(seed.rows);
  const [allowExtra, setAllowExtra] = useState<boolean>(seed.allowExtra);
  const [schemaText, setSchemaText] = useState<string>(() => schemaToText(task.output_schema));
  const [parseError, setParseError] = useState<string | null>(null);
  const [exampleText, setExampleText] = useState<string>("");
  const [exampleError, setExampleError] = useState<string | null>(null);

  // Re-seed everything when a different task is selected (keyed on id); leave in-progress edits
  // untouched across same-task re-renders.
  useEffect(() => {
    const next = parseSchemaToRows(task.output_schema);
    setRows(next.rows);
    setAllowExtra(next.allowExtra);
    setSchemaText(schemaToText(task.output_schema));
    setParseError(null);
    setExampleText("");
    setExampleError(null);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [task.id]);

  // Commit a built schema: push to the task and mirror into the raw-text editor so both stay in sync.
  const commitFromBuilder = (nextRows: FieldRow[], nextAllowExtra: boolean) => {
    const hasAnyField = nextRows.some((r) => r.name.trim().length > 0);
    if (!hasAnyField)
    {
      setSchemaText("");
      setParseError(null);
      onChange({ output_schema: undefined } as Partial<JcwfTask>);
      return;
    }
    const schema = rowsToSchema(nextRows, nextAllowExtra);
    setSchemaText(JSON.stringify(schema, null, 2));
    setParseError(null);
    onChange({ output_schema: schema } as Partial<JcwfTask>);
  };

  const updateRow = (idx: number, patch: Partial<FieldRow>) => {
    const next = rows.map((r, i) => (i === idx ? { ...r, ...patch } : r));
    setRows(next);
    commitFromBuilder(next, allowExtra);
  };

  const addRow = () => {
    const next = [...rows, { name: "", type: "string", required: false, enumText: "" }];
    setRows(next);
    // No commit yet — an empty-named row contributes nothing until it's named.
  };

  const removeRow = (idx: number) => {
    const next = rows.filter((_, i) => i !== idx);
    setRows(next);
    commitFromBuilder(next, allowExtra);
  };

  const toggleAllowExtra = (value: boolean) => {
    setAllowExtra(value);
    commitFromBuilder(rows, value);
  };

  // Raw-text editor: keep its own text so intermediate invalid JSON survives keystrokes, commit the
  // parsed object only when it parses, and re-seed the builder rows from it.
  const handleSchemaChange = (text: string) => {
    setSchemaText(text);
    if (text.trim().length === 0)
    {
      setParseError(null);
      setRows([]);
      onChange({ output_schema: undefined } as Partial<JcwfTask>);
      return;
    }
    try
    {
      const parsed = JSON.parse(text);
      setParseError(null);
      const reseed = parseSchemaToRows(parsed);
      setRows(reseed.rows);
      setAllowExtra(reseed.allowExtra);
      onChange({ output_schema: parsed } as Partial<JcwfTask>);
    }
    catch (e)
    {
      // Keep the raw text on screen; don't commit unparseable JSON to the task.
      setParseError(e instanceof Error ? e.message : "Invalid JSON");
    }
  };

  // Infer a schema from a pasted example answer (area F). On success, route the schema through the
  // existing commit path so the builder rows + raw textarea re-seed from it — no new plumbing.
  const handleInferFromExample = () => {
    const result = inferSchemaFromExample(exampleText);
    if (!result.ok)
    {
      setExampleError(result.error);
      return;
    }
    setExampleError(null);
    handleSchemaChange(JSON.stringify(result.schema, null, 2));
  };

  const retries = typeof task.output_retries === "number" ? task.output_retries : undefined;
  const hasSchema = task.output_schema !== undefined && task.output_schema !== null;

  return (
    <details className="field" style={{ borderLeft: "2px solid rgba(100,210,180,0.4)", paddingLeft: 8 }} open={hasSchema}>
      <summary style={{ cursor: "pointer", fontSize: 12, color: "rgba(100,210,180,0.95)" }}>
        Structured output (output_schema){hasSchema ? " ✓" : ""}
      </summary>
      <div className="small" style={{ marginTop: 6, opacity: 0.8 }}>
        Validates the AI reply against a JSON Schema; on mismatch the runtime re-asks (up to the
        retry budget). A validated reply lands at <code>&lt;stem&gt;.output.json</code> instead of
        <code>.output.txt</code>.
      </div>

      <details className="field" style={{ marginTop: 6 }}>
        <summary style={{ cursor: "pointer", fontSize: 11, opacity: 0.85 }}>
          Start from an example answer
        </summary>
        <div className="small" style={{ marginTop: 4, opacity: 0.7 }}>
          Paste one example of what a good reply looks like — a JSON object — and the fields below are
          filled in for you. Numbers become integer/number by the example; tune anything afterward.
        </div>
        <textarea
          className="input"
          style={{ fontFamily: "monospace", fontSize: 11, marginTop: 4 }}
          rows={5}
          value={exampleText}
          placeholder={'{\n  "category": "engine",\n  "confidence": 0.9\n}'}
          onChange={(e) => { setExampleText(e.target.value); setExampleError(null); }}
        />
        {exampleError && (
          <div className="small" style={{ color: "#ff8a8a", marginTop: 2 }}>{exampleError}</div>
        )}
        <button
          className="btn"
          type="button"
          style={{ padding: "3px 8px", fontSize: 11, marginTop: 4 }}
          disabled={exampleText.trim().length === 0}
          onClick={handleInferFromExample}
        >
          Infer fields from example
        </button>
      </details>

      <div className="field" style={{ marginTop: 6 }}>
        <div className="small" style={{ fontWeight: 600 }}>Fields</div>
        {rows.length === 0 && (
          <div className="small" style={{ opacity: 0.6 }}>No fields yet — add one, or paste a schema in the advanced editor below.</div>
        )}
        {rows.map((row, idx) => (
          <div key={idx} style={{ display: "flex", gap: 4, alignItems: "center", flexWrap: "wrap", marginTop: 4 }}>
            <input
              className="input"
              style={{ fontSize: 11, padding: "3px 6px", flex: "1 1 90px", minWidth: 80 }}
              value={row.name}
              placeholder="field name"
              onChange={(e) => { updateRow(idx, { name: e.target.value }); }}
            />
            <select
              className="input"
              style={{ fontSize: 11, padding: "3px 4px", width: 86 }}
              value={row.type}
              onChange={(e) => { updateRow(idx, { type: e.target.value }); }}
            >
              {FIELD_TYPES.map((t) => <option key={t} value={t}>{t}</option>)}
            </select>
            <label className="small" style={{ display: "inline-flex", alignItems: "center", gap: 3 }} title="Field must be present in the reply">
              <input type="checkbox" checked={row.required} onChange={(e) => { updateRow(idx, { required: e.target.checked }); }} />
              req
            </label>
            {row.type === "string" && (
              <input
                className="input"
                style={{ fontSize: 11, padding: "3px 6px", flex: "1 1 120px", minWidth: 90 }}
                value={row.enumText}
                placeholder="enum (a, b, c) — optional"
                title="Comma-separated allowed values; leave empty for any string"
                onChange={(e) => { updateRow(idx, { enumText: e.target.value }); }}
              />
            )}
            <button
              className="btn"
              type="button"
              style={{ padding: "2px 6px", fontSize: 10, color: "#ff8a8a" }}
              title="Remove field"
              onClick={() => { removeRow(idx); }}
            >x</button>
          </div>
        ))}
        <div style={{ display: "flex", gap: 8, alignItems: "center", marginTop: 6, flexWrap: "wrap" }}>
          <button className="btn" type="button" style={{ padding: "3px 8px", fontSize: 11 }} onClick={addRow}>+ field</button>
          <label className="small" style={{ display: "inline-flex", alignItems: "center", gap: 4 }} title="When off, the reply may not contain any keys beyond those declared (additionalProperties: false)">
            <input type="checkbox" checked={allowExtra} onChange={(e) => { toggleAllowExtra(e.target.checked); }} />
            allow extra fields
          </label>
        </div>
      </div>

      <details className="field" style={{ marginTop: 6 }}>
        <summary style={{ cursor: "pointer", fontSize: 11, opacity: 0.8 }}>Advanced — raw JSON Schema</summary>
        <textarea
          className="input"
          style={{ fontFamily: "monospace", fontSize: 11, marginTop: 4 }}
          rows={8}
          value={schemaText}
          placeholder={EXAMPLE_SCHEMA}
          onChange={(e) => { handleSchemaChange(e.target.value); }}
        />
        {parseError && (
          <div className="small" style={{ color: "#ff8a8a" }}>Invalid JSON — not saved: {parseError}</div>
        )}
        {schemaText.trim().length === 0 && (
          <button
            className="btn"
            type="button"
            style={{ padding: "3px 8px", fontSize: 11, marginTop: 4 }}
            onClick={() => { handleSchemaChange(EXAMPLE_SCHEMA); }}
          >
            Insert example schema
          </button>
        )}
      </details>

      <label className="field" style={{ marginTop: 6 }}>
        <div className="small">output_retries</div>
        <input
          className="input"
          type="number"
          min={0}
          value={retries ?? ""}
          placeholder="default 3"
          onChange={(e) => {
            const v = e.target.value;
            onChange({ output_retries: v === "" ? undefined : Math.max(0, parseInt(v, 10) || 0) } as Partial<JcwfTask>);
          }}
        />
      </label>
    </details>
  );
}
