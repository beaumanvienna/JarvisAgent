// Golden round-trip test (editor refactor, S7). The regression net for the Phase-2 save-path rewrite
// (U1 save-time derivation, U2 unified ports): graphToJcwf must be IDEMPOTENT — once jcwfToGraph →
// graphToJcwf has applied its one-time normalizations (ai_call env defaults, inline-with-ref-path →
// ref, dataflow dedupe, id sort, version, editor_layout), a second round-trip must produce a byte-
// identical canvas. A non-idempotent save means the editor keeps mutating a workflow on every open/
// save, which is exactly the drift the refactor must not introduce.
//
//   save(load(save(load(canvas)))) === save(load(canvas))
//
// Runs over the five curated example workflows. Reads each extracted canvas from the runtime
// workflows/<id>/<id>.json (the same shape jcwfToGraph receives from GET /api/workflows/<id>).
//
// Run:  npx tsx test/goldenRoundtrip.mts        (from code/frontend/workflow-editor/ui/)
// Exit: 0 = all idempotent, 1 = a drift was found (the first differing JSON path is printed).

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { jcwfToGraph } from "../src/editor/jcwfToGraph.js";
import { graphToJcwf } from "../src/editor/graphToJcwf.js";
import type { JcwfFile } from "../src/jcwf/types.js";

const EXAMPLES = [
  "aiCarMaintenancePipeline",
  "aiZipDemo",
  "make-example",
  "portfolioDividendAnalysis",
  "vehicleTroubleshootingGuide",
];

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(scriptDir, "../../../../..");

// Recursively sort object keys so the comparison is about VALUES, not cosmetic key order.
function canonical(value: unknown): unknown
{
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object")
  {
    const out: Record<string, unknown> = {};
    for (const k of Object.keys(value as Record<string, unknown>).sort())
    {
      out[k] = canonical((value as Record<string, unknown>)[k]);
    }
    return out;
  }
  return value;
}

// First differing JSON path between two canonicalized values (or null if equal).
function firstDiff(a: unknown, b: unknown, path = "$"): string | null
{
  if (JSON.stringify(a) === JSON.stringify(b)) return null;
  if (a && b && typeof a === "object" && typeof b === "object")
  {
    const ao = a as Record<string, unknown>;
    const bo = b as Record<string, unknown>;
    const keys = Array.from(new Set([...Object.keys(ao), ...Object.keys(bo)]));
    for (const k of keys)
    {
      const d = firstDiff(ao[k], bo[k], `${path}.${k}`);
      if (d) return d;
    }
  }
  return `${path}  (a=${JSON.stringify(a)?.slice(0, 80)}  b=${JSON.stringify(b)?.slice(0, 80)})`;
}

function save(canvas: JcwfFile, id: string): JcwfFile
{
  const result = graphToJcwf(jcwfToGraph(canvas, id), id);
  if (!result.ok) throw new Error(`graphToJcwf failed for '${id}': ${result.message} [${result.cycleNodes.join(", ")}]`);
  return result.jcwf;
}

let failed = 0;
for (const id of EXAMPLES)
{
  try
  {
    const canvasPath = resolve(repoRoot, "workflows", id, `${id}.json`);
    const canvas = JSON.parse(readFileSync(canvasPath, "utf8")) as JcwfFile;
    const raw1 = save(canvas, id);
    const s1 = canonical(raw1);
    const s2 = canonical(save(raw1, id));

    // (1) Idempotency: a second round-trip must produce identical output (S7).
    const diff = firstDiff(s1, s2);

    // (2) Path-mirror contract: U1 derives file_inputs at save time from the edges; for the SHIPPED
    // examples (whose stored paths the runtime resolves correctly) the derivation must reproduce the
    // exact stored file_inputs. A drift here means the frontend path synth (deriveUpstreamOutputPaths /
    // the base-leaf-strip) no longer mirrors the backend TaskPathResolver — the bug that would silently
    // corrupt a workflow on save.
    const srcTasks = (canvas.tasks ?? {}) as Record<string, { file_inputs?: unknown }>;
    const outTasks = (raw1.tasks ?? {}) as Record<string, { file_inputs?: unknown }>;
    const mirrorDrift = Object.keys(srcTasks).find((t) =>
      JSON.stringify(srcTasks[t]?.file_inputs ?? null) !== JSON.stringify(outTasks[t]?.file_inputs ?? null));

    if (diff)
    {
      console.log(`\x1b[91m✗\x1b[0m ${id}: NOT idempotent — first drift at ${diff}`);
      failed += 1;
    }
    else if (mirrorDrift)
    {
      console.log(`\x1b[91m✗\x1b[0m ${id}: file_inputs drift on save at task '${mirrorDrift}' — `
        + `stored=${JSON.stringify(srcTasks[mirrorDrift]?.file_inputs)} `
        + `derived=${JSON.stringify(outTasks[mirrorDrift]?.file_inputs)} (path mirror broke)`);
      failed += 1;
    }
    else
    {
      console.log(`\x1b[92m✓\x1b[0m ${id}: idempotent + file_inputs preserved (path mirror holds)`);
    }
  }
  catch (e)
  {
    console.log(`\x1b[91m✗\x1b[0m ${id}: ${e instanceof Error ? e.message : String(e)}`);
    failed += 1;
  }
}

console.log(failed === 0
  ? `\n\x1b[92mAll ${EXAMPLES.length} example workflows round-trip idempotently.\x1b[0m`
  : `\n\x1b[91m${failed}/${EXAMPLES.length} workflows drifted.\x1b[0m`);
process.exit(failed === 0 ? 0 : 1);
