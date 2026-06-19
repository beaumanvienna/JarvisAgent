// Phase 3 / area J (S6) — the no-code fan-out binding generator. Turns a choice of CSV columns into
// the inline `prob_files` entry a per_item ai_call needs, so the user never types a `{{binding.field}}`
// template, never has to know the per-row-filename-uniqueness requirement (F-40), and never sees the
// inline/ref distinction. `binding` is the filter's binding namespace (e.g. "pos" → `{{pos.Symbol}}`).
//
// Pure — no React / IO.

export type ProbBinding = { path: string; content: string };

// Generate the prob_files entry from the selected columns.
//   path:    PROB_{{<binding>.<firstCol>}}_{{<binding>.row_number_padded}}.txt
//            — the trailing _{{…row_number_padded}} guarantees ONE file per row even if the first
//              column repeats, so F-40's all-rows-collide-onto-one-file bug cannot recur by construction.
//   content: one "<Col>: {{<binding>.<Col>}}" line per selected column.
export function buildFanoutBinding(binding: string, selectedColumns: string[]): ProbBinding
{
  const ns = binding.trim().length > 0 ? binding.trim() : "item";
  const cols = selectedColumns.map((c) => c.trim()).filter((c) => c.length > 0);
  const firstCol = cols.length > 0 ? cols[0] : "item";

  const path = `PROB_{{${ns}.${firstCol}}}_{{${ns}.row_number_padded}}.txt`;
  const content = cols.map((c) => `${c}: {{${ns}.${c}}}`).join("\n") + (cols.length > 0 ? "\n" : "");

  return { path, content };
}

// Substitute one CSV data row into a generated binding so the UI can show what each call receives.
// `row` maps column name → value. `{{<binding>.row_number_padded}}` becomes a sample "001".
export function previewFanoutBinding(entry: ProbBinding, binding: string, row: Record<string, string>): ProbBinding
{
  const ns = binding.trim().length > 0 ? binding.trim() : "item";
  const substitute = (text: string): string =>
    text.replace(new RegExp(`\\{\\{\\s*${escapeRegExp(ns)}\\.([A-Za-z0-9_]+)\\s*\\}\\}`, "g"), (_m, field: string) => {
      if (field === "row_number_padded") return "001";
      return row[field] ?? `{{${ns}.${field}}}`;
    });
  return { path: substitute(entry.path), content: substitute(entry.content) };
}

function escapeRegExp(s: string): string
{
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}
