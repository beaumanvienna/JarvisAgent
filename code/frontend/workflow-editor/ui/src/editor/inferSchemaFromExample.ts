// Pure assist helper (U6 / area F). Maps an example JSON *object* — "what should one answer look
// like?" — to a Draft-2020-12 subset schema (the same subset the StructuredOutputEditor builder and
// the backend SchemaValidator support: type / enum / required / properties / additionalProperties /
// items). This is how a non-programmer thinks: by example, not by schema. The inferred schema is fed
// straight through the existing StructuredOutputEditor commit path, so the builder rows + raw
// textarea re-seed from it.
//
// No React / IO — unit-testable in isolation.

type JsonSchema = Record<string, unknown>;

// Schema for a single example value. Coarse by design — the raw textarea remains for hand-tuning:
// - null      → no constraint `{}` (and the key is omitted from `required` by the object branch)
// - boolean   → { type: "boolean" }
// - number    → integer vs number by the example value (heuristic; widen in the builder)
// - string    → { type: "string" }  (a single example is not an enum — enums are builder-only)
// - array     → items inferred from the first element; empty array → no items constraint
// - object    → properties recursed; required = keys with a non-null value; additionalProperties:false
function schemaOf(value: unknown): JsonSchema
{
  if (value === null)
  {
    return {};
  }
  if (typeof value === "boolean")
  {
    return { type: "boolean" };
  }
  if (typeof value === "number")
  {
    return { type: Number.isInteger(value) ? "integer" : "number" };
  }
  if (typeof value === "string")
  {
    return { type: "string" };
  }
  if (Array.isArray(value))
  {
    if (value.length === 0)
    {
      return { type: "array" };
    }
    return { type: "array", items: schemaOf(value[0]) };
  }
  if (typeof value === "object")
  {
    return objectSchema(value as Record<string, unknown>);
  }
  // undefined / function / symbol — unreachable for parsed JSON; no constraint.
  return {};
}

function objectSchema(obj: Record<string, unknown>): JsonSchema
{
  const properties: Record<string, JsonSchema> = {};
  const required: string[] = [];
  for (const [key, val] of Object.entries(obj))
  {
    properties[key] = schemaOf(val);
    // A null example means "optional / unknown" — present in properties, absent from required.
    if (val !== null)
    {
      required.push(key);
    }
  }
  const schema: JsonSchema = { type: "object", properties };
  if (required.length > 0)
  {
    schema.required = required;
  }
  schema.additionalProperties = false;
  return schema;
}

export type InferResult =
  | { ok: true; schema: JsonSchema }
  | { ok: false; error: string };

// Parse the example text and infer a schema. The top level MUST be a JSON object (one example
// answer); anything else (array, scalar, invalid JSON) returns an actionable hint rather than a
// degenerate schema.
export function inferSchemaFromExample(exampleText: string): InferResult
{
  const trimmed = exampleText.trim();
  if (trimmed.length === 0)
  {
    return { ok: false, error: "Paste an example JSON object — one example answer." };
  }

  let parsed: unknown;
  try
  {
    parsed = JSON.parse(trimmed);
  }
  catch (e)
  {
    return { ok: false, error: `Not valid JSON: ${e instanceof Error ? e.message : "parse error"}` };
  }

  if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed))
  {
    return { ok: false, error: "Paste a JSON object — one example answer (e.g. { \"category\": \"engine\" })." };
  }

  return { ok: true, schema: objectSchema(parsed as Record<string, unknown>) };
}
