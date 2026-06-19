// U2 — one classifier for every editor edge (S5). Instead of migrating the handle-id grammar
// (`dephandle-N` / `fileoutput-N` / `in:` / `out:` / `cf-*`), we unify at the classify + serialize
// layer: this single function decides what an edge MEANS from its handles + endpoints, and both the
// connect path (`onConnect`) and the save path (`graphToJcwf`) route through it. The `dep:` / `df:` /
// `cf:` / `fanout:` / `file:` edge-id prefixes are just the persisted encoding of this result.
//
// Pure — no React / IO.

export type EdgeClass = "fileflow" | "dataflow" | "controlflow" | "fanout";

// `fileflow` covers BOTH a task→task dependency (serialises to depends_on + derived file_inputs) and
// an artifact-file-node → task hand-off (a pure editor overlay the save path skips). The two are told
// apart by the source id (`file:` prefix), not by this classifier — keep that check at the call site.
export function classifyEdge(
  sourceHandle: string | null | undefined,
  targetHandle: string | null | undefined,
  sourceId: string,
  targetId: string,
): EdgeClass
{
  void targetId; // reserved for future capability-aware classification (S4)

  // Named value slots — the dataflow edge.
  if (sourceHandle?.startsWith("out:") || targetHandle?.startsWith("in:"))
  {
    return "dataflow";
  }

  // Branch / error-signal ports — the control-flow edge.
  if (sourceHandle === "error-signal" || sourceHandle === "cf-out-normal" || sourceHandle === "cf-out-error"
    || targetHandle === "cf-in-normal" || targetHandle === "cf-in-error")
  {
    return "controlflow";
  }

  // A filter node feeding a per_item task — the fan-out edge (binding lives in task.filter).
  if (sourceId.startsWith("filter:"))
  {
    return "fanout";
  }

  // Everything else between two wire-able endpoints (a task output / file node → a task input) is a
  // file/dependency hand-off.
  return "fileflow";
}
