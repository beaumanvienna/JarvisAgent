# FileWriter

## Overview

Thread-safe file output helper used by `AiRequestPool::Submit` to write
`.output.{txt,json}` artefacts for `ai_call` tasks. Serialises all writes
through a single `std::mutex` so multiple in-flight AI replies landing in the
same folder cannot interleave file writes.

---

## Write(path, content)
- Acquire the per-instance lock.
- Route `content` through `EngineCore::AtomicWriteFile` (`engine/auxiliary/file.h`),
  which creates missing parent directories, opens `<path>.tmp.<atomic-counter>`
  with `failbit | badbit` exceptions, writes, closes, then `fs::rename`s to
  the final path.  A SIGKILL or disk-full mid-write leaves the previous
  version intact rather than a truncated partial — these queue files
  (STNG / CNTX / TASK / PROB) are what AI dispatch consumes as completion
  signals, so a torn write would surface as a malformed dispatch input.
- Log `LOG_APP_INFO` on success with the resolved path; `LOG_APP_ERROR` on
  failure with the helper's populated `errorMessage`.

## WriteWithHeader(path, content, model)
Despite the name, this method currently writes `content` exactly like
`Write` — the `model` parameter is accepted but not consumed, and no header
lines are prepended.

- Acquire the per-instance lock.
- Same `EngineCore::AtomicWriteFile` path as `Write`.
- Log `LOG_APP_INFO` on success; `LOG_APP_ERROR` on failure.

Call sites live in `aiRequestPool.cpp` (`.output.txt` / `.output.json`
writers). Keeping the two methods separate today preserves a future
extension point if per-reply provenance headers (model, timestamp, finish
reason) ever need to be baked into the artefact itself rather than into the
sidecar transcript.
