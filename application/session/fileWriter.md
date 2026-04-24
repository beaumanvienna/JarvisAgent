# FileWriter

## Overview

Thread-safe file output helper used by `AiRequestPool::Submit` to write
`.output.{txt,json}` artefacts for `ai_call` tasks. Serialises all writes
through a single `std::mutex` so multiple in-flight AI replies landing in the
same folder cannot interleave file writes.

---

## Write(path, content)
- Acquire the per-instance lock.
- Create any missing parent directories (absolute + lexically-normalised).
- Open the file with `std::ios::trunc` and write `content` verbatim — no
  transformation, no trailing newline added.
- Log success or open/write failure with the resolved path.

## WriteWithHeader(path, content, model)
Despite the name, this method currently writes `content` exactly like
`Write` — the `model` parameter is accepted but not consumed, and no header
lines are prepended.

- Acquire the per-instance lock.
- Create any missing parent directories.
- Truncate + write `content` verbatim.
- Log success or open/write failure.

Call sites live in `aiRequestPool.cpp` (`.output.txt` / `.output.json`
writers). Keeping the two methods separate today preserves a future
extension point if per-reply provenance headers (model, timestamp, finish
reason) ever need to be baked into the artefact itself rather than into the
sidecar transcript.
