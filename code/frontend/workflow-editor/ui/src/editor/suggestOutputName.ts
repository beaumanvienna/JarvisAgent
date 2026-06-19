// Pure assist helper (U6 / area G). Given an input file's basename, suggest a conventional output
// name `<stem>.output.<ext>` — the same `.output.<ext>` shape the runtime uses for AI replies and
// the convention downstream tasks expect. The editor offers this as a one-click chip so the user
// accepts a sensible default instead of typing an output filename from scratch.
//
// No React / IO — unit-testable in isolation.

// Strip any directory portion and return the trailing path segment.
function basenameOf(path: string): string
{
  const segs = path.split(/[\\/]/);
  return segs[segs.length - 1] ?? path;
}

// Split a basename into stem + extension (extension WITHOUT the dot, lowercased preserved as-is).
// A leading dot (dotfile) is treated as part of the stem, not an extension separator.
function splitStemExt(basename: string): { stem: string; ext: string }
{
  const dot = basename.lastIndexOf(".");
  if (dot <= 0)
  {
    // No extension, or a dotfile like ".env" — no meaningful extension to carry.
    return { stem: basename, ext: "" };
  }
  return { stem: basename.slice(0, dot), ext: basename.slice(dot + 1) };
}

// Suggest `<stem>.output.<ext>` from an input path/basename.
// - The input's extension is carried through (`analysis.txt` → `analysis.output.txt`).
// - No extension → defaults to `.txt` (the runtime's plain-reply default).
// - If the input already looks like an output (`foo.output.txt`), it is returned unchanged so the
//   suggestion never compounds into `foo.output.output.txt`.
// Returns an empty string for an empty/whitespace input (nothing to suggest).
export function suggestOutputName(inputPath: string): string
{
  const trimmed = inputPath.trim();
  if (trimmed.length === 0)
  {
    return "";
  }

  const basename = basenameOf(trimmed);
  const { stem, ext } = splitStemExt(basename);

  // Already an output-shaped name — don't double the `.output` segment.
  if (stem.endsWith(".output"))
  {
    return basename;
  }

  const effectiveExt = ext.length > 0 ? ext : "txt";
  return `${stem}.output.${effectiveExt}`;
}
