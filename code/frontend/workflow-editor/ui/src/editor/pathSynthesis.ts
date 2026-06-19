// U1 — the path authority (S2). Resolves the concrete relative path a downstream task reads for an
// upstream task's output(s), correct through ".." traversals, mirroring the backend TaskPathResolver
// contract (file_inputs resolved relative to the task working dir; AI replies at
// `<prob_stem>.output.{txt,json}`). Shared by the edge-draw auto-populate (onConnect), the input
// re-sync, and the inspector's wired-input suggestions so the three never drift.
//
// Pure — no React / IO. The eventual Phase-2 move is to call this *inside* graphToJcwf so file_inputs
// is derived at save time (killing the stored-path drift cluster); for now it backs the existing
// eager call sites.

import type { JcwfTask } from "../jcwf/types";

// Lexically normalize a POSIX path into segments, resolving "." and ".." (keeping leading ".." that
// climb above the start). No filesystem access.
export function normalizePathSegments(path: string): string[]
{
  const out: string[] = [];
  for (const seg of path.split("/"))
  {
    if (seg === "" || seg === ".") continue;
    if (seg === "..")
    {
      if (out.length > 0 && out[out.length - 1] !== "..") out.pop();
      else out.push("..");
    }
    else out.push(seg);
  }
  return out;
}

// Relative POSIX path from a directory to a file, both given as normalized segment lists.
export function relativePathBetween(fromDir: string[], toPath: string[]): string
{
  let i = 0;
  while (i < fromDir.length && i < toPath.length && fromDir[i] === toPath[i]) i++;
  const ups = fromDir.length - i;
  const parts = [...Array<string>(ups).fill(".."), ...toPath.slice(i)];
  return parts.join("/");
}

// The AI-reply convention: a PROB file `<stem>.<ext>` produces `<stem>.output.<ext2>` where ext2 is
// `json` when the task declares an output_schema (validated reply lands as JSON), else `txt`. This is
// the single encoding of the backend convention (aiCallTaskExecutor.cpp). Replaces the previous
// `.replace(/\.txt$/, ".output.txt")` which both hardcoded `.txt` inputs AND ignored output_schema.
function probToOutputName(probPath: string, ext: "json" | "txt"): string
{
  const slash = probPath.lastIndexOf("/");
  const dir = slash >= 0 ? probPath.slice(0, slash + 1) : "";
  const file = slash >= 0 ? probPath.slice(slash + 1) : probPath;
  const dot = file.lastIndexOf(".");
  const stem = dot > 0 ? file.slice(0, dot) : file;
  return `${dir}${stem}.output.${ext}`;
}

// A per_item ai_call produces MANY outputs (one per row: `PROB_<sym>_<NN>.output.<ext>`), so a
// consumer references them with a glob rather than a single file — exactly the shipped portfolioSummary
// cntx pattern `PROB_*.output.json`. The glob keeps any directory prefix and the literal filename
// prefix up to the first `{{template}}`, replacing the varying tail with `*`. A per_item path with no
// template (degenerate — all rows collide, flagged elsewhere) falls back to the single-file name.
function probToOutputGlob(probPath: string, ext: "json" | "txt"): string
{
  const slash = probPath.lastIndexOf("/");
  const dir = slash >= 0 ? probPath.slice(0, slash + 1) : "";
  const file = slash >= 0 ? probPath.slice(slash + 1) : probPath;
  const tpl = file.indexOf("{{");
  if (tpl < 0)
  {
    return probToOutputName(probPath, ext);
  }
  const literalPrefix = file.slice(0, tpl);
  return `${dir}${literalPrefix}*.output.${ext}`;
}

// Resolve a task's working_directory to a workflow-base-relative segment list, mirroring the backend
// `TaskPathResolver::ResolvePath`'s base-leaf-strip rule (taskPathResolver.cpp): a wd is normally
// resolved against the workflow base `workflows/<wfId>/`, EXCEPT when its first segment already equals
// the workflow-folder leaf (`<wfId>`) — then it resolves against the base's PARENT (`workflows/`) so
// the leaf isn't duplicated. This is why aiZipDemo's shell wd "aiZipDemo/04_zip_responses" resolves to
// workflows/aiZipDemo/04_zip_responses, NOT the doubled workflows/aiZipDemo/aiZipDemo/04_zip_responses.
// Without this, a save-time path derivation drifts by one ".." for any task using the `<wfId>/…` form.
export function resolveTaskDirSegments(wfId: string, wd: string): string[]
{
  const firstSeg = wd.split("/").find((s) => s.length > 0);
  // baseLeaf === wfId; first segment matches → resolve under the base parent (workflows/), else base.
  return firstSeg === wfId
    ? normalizePathSegments(`workflows/${wd}`)
    : normalizePathSegments(`workflows/${wfId}/${wd}`);
}

// The `file_inputs` string a consumer task should store for an artifact-file node it is wired to:
// the relative path from the consumer's RESOLVED working directory to the file. The file node's path
// is relative to the workflow folder (workflows/<wfId>/); the consumer dir is resolved through the
// same base-leaf-strip the runtime uses (resolveTaskDirSegments) so the result round-trips with the
// backend resolver AND with the load-time inference (S3) for any wd convention — incl. the `<wfId>/…`
// shell form. (U3 file-node creation / onConnect; U1 save-time derivation.)
export function fileNodeInputPath(workflowRelPath: string, consumerWd: string, wfId: string): string
{
  const consumerDir = resolveTaskDirSegments(wfId, consumerWd);
  const fileAbs = normalizePathSegments(`workflows/${wfId}/${workflowRelPath}`);
  return relativePathBetween(consumerDir, fileAbs);
}

// The file path(s) a downstream task reads for an upstream task's output(s). Both working_directories
// are resolved against the workflow base (workflows/<wfId>/) and the relative path between them is
// taken — correct even when a wd contains ".." (e.g. the auto-filled ../../queue/<wf>/<task>).
//   - ai_call source: declared file_outputs (e.g. classification.output.json), else the disk-first
//     PROB → `.output.{json,txt}` convention (json when output_schema is set). A per_item ai_call
//     emits a single `PROB_*.output.<ext>` glob instead of one path per prob entry.
//   - shell/python/internal source: file_outputs, narrowed to the dragged fileoutput-N if given.
export function deriveUpstreamOutputPaths(sourceTask: JcwfTask, targetTask: JcwfTask, wfId: string, sourceHandle?: string | null): string[]
{
  const sourceWd = (sourceTask.working_directory ?? "") as string;
  const sourceDirAbs = resolveTaskDirSegments(wfId, sourceWd);
  const targetDirAbs = resolveTaskDirSegments(wfId, (targetTask.working_directory ?? "") as string);

  let outputNames: string[] = [];
  if (sourceTask.type === "ai_call")
  {
    const aiFileOutputs = Array.isArray(sourceTask.file_outputs)
      ? (sourceTask.file_outputs as string[]).filter((o) => o.trim().length > 0)
      : [];
    if (aiFileOutputs.length > 0)
    {
      outputNames = aiFileOutputs;
    }
    else
    {
      const ext: "json" | "txt" = sourceTask.output_schema !== undefined && sourceTask.output_schema !== null ? "json" : "txt";
      const qb = (sourceTask.queue_binding ?? {}) as Record<string, unknown>;
      const probFiles = (qb.prob_files ?? []) as Array<{ path: string } | string>;
      const perItem = sourceTask.mode === "per_item";
      const names = probFiles.map((entry) => {
        const p = typeof entry === "string" ? entry : entry.path;
        return perItem ? probToOutputGlob(p, ext) : probToOutputName(p, ext);
      });
      // A per_item producer's prob entries may collapse to the same glob — dedupe.
      outputNames = Array.from(new Set(names));
    }
  }
  else if (sourceTask.type === "shell" || sourceTask.type === "python" || sourceTask.type === "internal")
  {
    const sourceOutputs = Array.isArray(sourceTask.file_outputs)
      ? (sourceTask.file_outputs as string[]).filter((o) => o.trim().length > 0)
      : [];
    if (typeof sourceHandle === "string" && sourceHandle.startsWith("fileoutput-"))
    {
      const oi = parseInt(sourceHandle.slice(11), 10);
      outputNames = (oi >= 0 && oi < sourceOutputs.length) ? [sourceOutputs[oi]] : sourceOutputs;
    }
    else
    {
      outputNames = sourceOutputs;
    }
  }

  return outputNames
    .filter((name) => name.trim().length > 0)
    .map((name) => relativePathBetween(targetDirAbs, normalizePathSegments(`${sourceDirAbs.join("/")}/${name}`)));
}
