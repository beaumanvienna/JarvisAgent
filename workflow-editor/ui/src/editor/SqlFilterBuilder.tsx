import React, { useState } from "react";

type Operator = "=" | "!=" | ">" | ">=" | "<" | "<=" | "LIKE" | "NOT LIKE" | "IN" | "NOT IN" | "IS NULL" | "IS NOT NULL";

type FilterRow = {
  column: string;
  operator: Operator;
  value: string;
  connector: "AND" | "OR";
};

const OPERATORS: { value: Operator; label: string }[] = [
  { value: "=", label: "= equals" },
  { value: "!=", label: "!= not equal" },
  { value: ">", label: "> greater than" },
  { value: ">=", label: ">= greater or equal" },
  { value: "<", label: "< less than" },
  { value: "<=", label: "<= less or equal" },
  { value: "LIKE", label: "LIKE (pattern)" },
  { value: "NOT LIKE", label: "NOT LIKE" },
  { value: "IN", label: "IN (list)" },
  { value: "NOT IN", label: "NOT IN" },
  { value: "IS NULL", label: "IS NULL" },
  { value: "IS NOT NULL", label: "IS NOT NULL" },
];

const UNARY_OPS: Operator[] = ["IS NULL", "IS NOT NULL"];

function makeEmptyRow(): FilterRow
{
  return { column: "", operator: "=", value: "", connector: "AND" };
}

function isNumeric(value: string): boolean
{
  return /^-?\d+(\.\d+)?$/.test(value.trim());
}

function buildClause(rows: FilterRow[]): string
{
  const parts: string[] = [];

  for (let i = 0; i < rows.length; i++)
  {
    const row = rows[i];
    if (!row.column.trim())
    {
      continue;
    }

    if (parts.length > 0)
    {
      parts.push(row.connector);
    }

    const col = row.column.trim();

    if (UNARY_OPS.includes(row.operator))
    {
      parts.push(`${col} ${row.operator}`);
    }
    else if (row.operator === "IN" || row.operator === "NOT IN")
    {
      const items = row.value.split(",").map((v) =>
      {
        const trimmed = v.trim();
        return isNumeric(trimmed) ? trimmed : `'${trimmed.replace(/'/g, "''")}'`;
      }).join(", ");
      parts.push(`${col} ${row.operator} (${items})`);
    }
    else
    {
      const val = row.value.trim();
      const rhs = isNumeric(val) ? val : `'${val.replace(/'/g, "''")}'`;
      parts.push(`${col} ${row.operator} ${rhs}`);
    }
  }

  return parts.join(" ");
}

type Props = {
  query: string;
  onInsert: (clause: string) => void;
};

export default function SqlFilterBuilder({ query, onInsert }: Props): JSX.Element
{
  const [open, setOpen] = useState(false);
  const [rows, setRows] = useState<FilterRow[]>([makeEmptyRow()]);

  const updateRow = (index: number, patch: Partial<FilterRow>) =>
  {
    setRows((prev) => prev.map((r, i) => (i === index ? { ...r, ...patch } : r)));
  };

  const removeRow = (index: number) =>
  {
    setRows((prev) =>
    {
      if (prev.length <= 1)
      {
        return [makeEmptyRow()];
      }
      return prev.filter((_, i) => i !== index);
    });
  };

  const addRow = () =>
  {
    setRows((prev) => [...prev, makeEmptyRow()]);
  };

  const clause = buildClause(rows);
  const hasValidRows = rows.some((r) => r.column.trim() !== "");

  const handleInsert = () =>
  {
    if (clause)
    {
      onInsert(clause);
      setRows([makeEmptyRow()]);
    }
  };

  const accent = "rgba(34,197,94,0.95)";
  const accentDim = "rgba(34,197,94,0.5)";

  return (
    <div className="field" style={{ marginTop: 2, marginBottom: 2 }}>
      <button
        className="btn"
        style={{ fontSize: 11, padding: "2px 8px", opacity: 0.85 }}
        onClick={() => setOpen((v) => !v)}
        type="button"
      >
        {open ? "Hide" : "Show"} Filter Builder
      </button>

      {open && (
        <div style={{ marginTop: 6, padding: 6, border: `1px solid ${accentDim}`, borderRadius: 4 }}>
          {rows.map((row, i) => (
            <div key={i} style={{ display: "flex", gap: 4, alignItems: "center", marginBottom: 4, flexWrap: "wrap" }}>
              {i > 0 && (
                <select
                  className="input"
                  style={{ width: 56, fontSize: 11 }}
                  value={row.connector}
                  onChange={(e) => updateRow(i, { connector: e.target.value as "AND" | "OR" })}
                >
                  <option value="AND">AND</option>
                  <option value="OR">OR</option>
                </select>
              )}
              {i === 0 && <span style={{ width: 56, display: "inline-block" }} />}
              <input
                className="input"
                style={{ flex: 1, minWidth: 60, fontSize: 11, fontFamily: "monospace" }}
                placeholder="column"
                value={row.column}
                onChange={(e) => updateRow(i, { column: e.target.value })}
              />
              <select
                className="input"
                style={{ width: 110, fontSize: 11 }}
                value={row.operator}
                onChange={(e) => updateRow(i, { operator: e.target.value as Operator })}
              >
                {OPERATORS.map((op) => (
                  <option key={op.value} value={op.value}>{op.label}</option>
                ))}
              </select>
              {!UNARY_OPS.includes(row.operator) && (
                <input
                  className="input"
                  style={{ flex: 1, minWidth: 50, fontSize: 11, fontFamily: "monospace" }}
                  placeholder={row.operator === "IN" || row.operator === "NOT IN" ? "a, b, c" : "value"}
                  value={row.value}
                  onChange={(e) => updateRow(i, { value: e.target.value })}
                />
              )}
              <button
                className="btn"
                style={{ fontSize: 11, padding: "1px 6px", opacity: 0.7 }}
                onClick={() => removeRow(i)}
                type="button"
                title="Remove condition"
              >
                x
              </button>
            </div>
          ))}

          <div style={{ display: "flex", gap: 6, marginTop: 4 }}>
            <button className="btn" style={{ fontSize: 11, padding: "2px 8px" }} onClick={addRow} type="button">
              + Condition
            </button>
            <button
              className="btn"
              style={{ fontSize: 11, padding: "2px 8px", color: accent, borderColor: accentDim }}
              onClick={handleInsert}
              type="button"
              disabled={!hasValidRows}
              title={clause ? `Insert: ${clause}` : "Add at least one condition"}
            >
              Insert into query
            </button>
          </div>

          {hasValidRows && (
            <div style={{ marginTop: 4, fontSize: 11, fontFamily: "monospace", color: accent, opacity: 0.85, wordBreak: "break-all" }}>
              {clause}
            </div>
          )}
        </div>
      )}
    </div>
  );
}
