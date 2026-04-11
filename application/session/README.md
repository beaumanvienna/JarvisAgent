# Session Manager

Monitors queue folders for file-based AI request sessions. Each session assembles STNG (settings), CNTX (context), TASK, and PROB (problem/prompt) files, dispatches AI requests, and writes response files.

## Key Files

| File | Purpose |
|------|---------|
| `sessionManager.h/cpp` | Core session lifecycle — file detection, assembly, dispatch, completion |
| `sessionManager_fileWriter.md` | Detailed documentation of the file-writing pipeline |

## Data Flow

1. **File Watcher** detects new files in queue folders
2. **File Categorizer** classifies by type (STNG, CNTX, TASK, PROB, PROV)
3. **Session Manager** assembles the environment (STNG + CNTX + TASK)
4. **AI Request Pool** dispatches queries in parallel (one per PROB file)
5. Response files are written back to the queue folder

## Queue File Types

| Prefix | Purpose |
|--------|---------|
| `STNG_` | Settings (tone, style) |
| `CNTX_` | Context (background knowledge) |
| `TASK_` | Task instructions |
| `PROB_` | Problem/prompt (triggers AI request) |
| `PROV_` | Provider override |
