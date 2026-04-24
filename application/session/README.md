# Session

Disk-first helpers for the envelope-driven AI dispatch path (see
`application/workflow/aiRequestPool.cpp`).

## Key Files

| File | Purpose |
|------|---------|
| `fileWriter.h/cpp` | Atomic `.output.{txt,json}` writer used by `AiRequestPool::Submit` |
| `fileWriter.md` | Detailed documentation of the file-writing pipeline |

## Queue File Types

`ai_call` tasks write these alongside `.output.*` / `.transcript.json` for replay
and debugging — they are no longer load-bearing for dispatch.

| Prefix | Purpose |
|--------|---------|
| `STNG_` | Settings (tone, style) |
| `CNTX_` | Context (background knowledge) |
| `TASK_` | Task instructions |
| `PROB_` | Problem/prompt (one per fan-out item) |
| `PROV_` | Provider sidecar (optional — the envelope is authoritative; PROV is read only by replay tooling) |
