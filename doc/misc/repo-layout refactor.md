# Repo-layout hygiene refactor — plan

Goal: a clean root. Move all *source* under a single `code/` tree (`backend/`, `frontend/`, `mcp/`, `vendor/`), untrack the runtime scratch folders so they stop showing up empty on GitHub, relocate the Docker cluster and the two long-form docs out of the root, dissolve `tools/`, and delete a handful of stale/leftover files. Keep `test/`, `doc/`, `example/`, `integration/`, `packaging/`, `certs/`, `scripts/`, `.github/`, `.vscode/` where they are.

This supersedes the earlier `JarvisAgent TODO List.md` §5d sketch; that item is closed by this plan.

---

## Target layout

```
code/
  backend/
    engine/            (was engine/)
    application/       (was application/)
  frontend/
    dashboard/         (was dashboard/)
    workflow-editor/   (was workflow-editor/)
    shared-ui/         (was shared-ui/)
  mcp/                 (was mcp/)
  vendor/              (was vendor/)

doc/
  DEVELOPMENT.md       (moved from root)
  INSTALL.md           (moved from root)
  ...

packaging/
  Docker/
    Dockerfile               (moved from root)
    docker-compose.example.yml (moved from root)
    docker-entrypoint.sh     (moved from root)
  Linux/ ...

scripts/
  clang-format.sh      (moved from tools/, find-paths retargeted)
  ...

Root (tracked) — minimum set:
  .clang-format  .clangd  .dockerignore  .editorconfig  .gitignore
  CLAUDE.md  LICENSE  README.md  config.json  jarvisagent.sh  premake5.lua  todo.md
```

Runtime folders (`queue/`, `workflows/`, `log/`, `assistant/`, `_adhoc/`, `.npm-tools/`) become fully untracked — created on demand at runtime, never in git, never shown empty on GitHub.

---

## Why this is far less risky than the diff size suggests

Four properties, each verified against the current tree, mean the heaviest moves touch almost no content — they're `git mv` + a handful of build-config string edits:

1. **Backend move = zero C++ source edits.** There are **0** `#include "engine/..."` / `#include "application/..."` prefixed includes. Every backend include is relative-to-includedir (`#include "workflow/aiRequestPool.h"` resolves via `application/` on the search path). So pointing `premake5.lua`'s `includedirs` at the new locations is sufficient — no `.cpp`/`.h` edits.
2. **Frontend move = zero frontend-config edits.** The `@shared` alias is `../../shared-ui` relative to each UI dir, and `dashboard/`, `workflow-editor/`, `shared-ui/` all move under `code/frontend/` as **siblings**. `../../shared-ui` resolves identically before and after. vite + tsconfig need no change.
3. **Vendor move = zero vendor-script edits.** The six `vendor/*.lua` build scripts use **self-relative** paths (`curl/lib/...`, not `vendor/curl/...`). premake's `include` runs them in their own directory context, so only the `include "vendor/foo.lua"` lines (and the repo-root-relative clean/`files`/`includedirs` entries in the *main* script) change.
4. **mcp move = self-contained.** `mcp/` has no `../application` / `../engine` / `../shared-ui` references; it's a standalone TypeScript sidecar.

The real edit surface is concentrated in: `premake5.lua`, the two C++ static-serve constants, the packaging/CI build-and-copy scripts, `.gitignore`, and a doc sweep.

---

## Reference-count baseline (tracked files referencing each moving path)

| Path | files |  | Path | files |
|------|------:|--|------|------:|
| `application/` | 133 | | `mcp/` | 21 |
| `engine/` | 55 | | `tools/` | 12 |
| `vendor/` | 21 | | `DEVELOPMENT.md` | 9 |
| `dashboard/` | 38 | | `INSTALL.md` | 8 |
| `workflow-editor/` | 37 | | `todo.md` | 10 |
| `shared-ui` | 10 | | | |

Most of these are prose mentions in `doc/` + `CLAUDE.md`, not code. The doc sweep (Slice 7) is the long pole by reference count, not by risk.

---

## The one design decision: how the binary finds the UI `dist`

Today the binary serves the UIs from a **CWD-relative** hard-coded path — `std::filesystem::path("dashboard")/"ui"/"dist"` (`webServer.cpp:499,515`) and `workflow-editor/ui/dist` (`webServer_studio.cpp:58`). Packaging installs to a flat `<root>/dashboard/ui/dist`. After the move, the dev-tree build output lands at `code/frontend/dashboard/ui/dist`, so local `./jarvisagent.sh` would no longer find it.

**Recommendation — ordered-root resolver (decouples runtime from source layout).** Add a small helper that resolves the first existing of:
1. `dashboard/ui/dist` — installed/flat layout (all packaging keeps copying here, **unchanged**)
2. `code/frontend/dashboard/ui/dist` — dev source layout

~10 lines of C++, touches only the 4 serve call sites (index + static × 2 UIs), keeps every packaging script's install layout flat (no `code/` in shipped artifacts), and makes local dev "just work" from the new tree. Rejected alternatives: baking `code/frontend/...` into the served path (leaks a `code/` dir into installed packages); symlink shims in the launcher (fragile on Windows).

---

## Migration slices

Worked in order low-risk → high-risk so the tree stays buildable and verifiable at every step. **All slices land as one reorg commit** (JC's call) — the slicing is an execution/verification discipline, not a commit boundary; JC makes the single commit at the end.

### Slice 1 — Untrack runtime scratch folders
- **Prerequisite (DONE in code):** the backend must create the `queue/` + `workflows/` roots at startup, else a fresh checkout starts degraded. Verified it previously did **not** — `WorkflowRegistry::LoadDirectory` only warns-and-returns on a missing dir, and `queue/` was created lazily per-task. Added an idempotent `create_directories` block in `JarvisAgent::OnStart` (`application/jarvisAgent.cpp`, just before the workflow-file-index scan), driven by `config.m_QueueFolderFilepath` / `m_WorkflowsFolderFilepath`, logging `runtime root ready: '<root>'`. Mirrors the existing `assistant/sessions` create. Builds clean (studio/debug). **Live confirmation pending** — the TUI server binary can't be smoke-run in the sandbox shell; JC to confirm in a terminal (folders reappear on launch, log shows the two `runtime root ready` lines).
- `git rm -r --cached queue/ workflows/ log/ assistant/ .npm-tools/` (drops the per-dir `.gitignore` placeholders).
- Delete the leftover `.npm-tools/package.json` stub (the launcher `npm install --prefix`'s its own).
- Replace the per-dir `*`/`!.gitignore` trick with top-level `.gitignore` entries: `/queue/ /workflows/ /assistant/ /.npm-tools/` and tighten `log/*` → `/log/`. (`_adhoc` already ignored.)
- Note: `assistant/sessions` is already created at startup; `.npm-tools/` is created by `jarvisagent.sh`.

### Slice 2 — Root-file cleanup (no source moves)
- `git mv DEVELOPMENT.md INSTALL.md doc/`. `todo.md` **stays at root** (live-TODO convention unchanged).
- `git mv Dockerfile docker-compose.example.yml docker-entrypoint.sh packaging/Docker/`. **`.dockerignore` stays at root** — Docker reads it only from the build-context root, not from beside the Dockerfile.
  - Update touch points: CI `docker-publish.yml` (`file: ./Dockerfile` → `./packaging/Docker/Dockerfile`, context stays `.`), `scripts/run-docker.{sh,ps1}`, any Flatpak/AppImage snapshot. The `COPY` paths *inside* the Dockerfile also retarget to `code/frontend/...` (done in Slice 4).
- Delete `jarvis_agent.example.env` — stale: its `OPENAI_API_KEY` env-credential path was purged (encrypted-only keystore), leaving only a dev-local `TRACY_NO_INVARIANT_CHECK`.
- Dissolve `tools/`: `git mv tools/clang-format.sh scripts/`; `git rm tools/{clean,print_docs,print_files,test}.sh`; remove the now-empty `tools/`.
  - Retarget `clang-format.sh`'s `find` from `${repoRoot}/application ${repoRoot}/engine` → `${repoRoot}/code/backend/application ${repoRoot}/code/backend/engine` (its repoRoot derivation already survives the dir change).
- Delete `application/workflow/doc/aiCallArchitecture.md` — superseded by the AI-dispatch refactor; describes the removed file-watcher completion flow (no historical-note stub, per the no-legacy rule).

### Slice 3 — Backend → `code/backend/`
- `git mv engine code/backend/engine` ; `git mv application code/backend/application`.
- `premake5.lua` edits **only**:
  - `files`: `application/**` → `code/backend/application/**`, `engine/**` → `code/backend/engine/**`.
  - `removefiles` (Studio + Engine arms): same prefix retarget.
  - `includedirs`: `engine/` → `code/backend/engine/`, `application/` → `code/backend/application/`. (`test/`, `vendor/...` unchanged in this slice.)
  - `embedAsHeader` output paths: `application/json/*.generated.h` → `code/backend/application/json/*.generated.h`.
  - `--heapscan` `files`: `test/security/...` unchanged (test stays at root).
- `.gitignore`: `application/json/*.generated.h` → `code/backend/application/json/*.generated.h`.
- `clang-format.sh` find-paths (if not already done in Slice 2).
- **Verify:** full `premake5 clean` → 4 configs build warning-free. Zero `.cpp`/`.h` content edits expected — if the compiler can't find a header, an includedir was missed.

### Slice 4 — Frontend → `code/frontend/` (+ static-serve decouple)
- `git mv dashboard workflow-editor shared-ui code/frontend/`.
- Frontend configs unchanged (sibling property). Verify a clean `npm install && npm run build` in both UIs still resolves `@shared`.
- Implement the ordered-root resolver (design decision above); update the 4 serve call sites + the two "please run: cd dashboard/ui ..." messages to `cd code/frontend/dashboard/ui ...`.
- Retarget build-and-copy references in: CI `linux-workflow.yml` (build steps + artifact paths), `Dockerfile` `COPY` lines, `packaging/Linux/{AppImage,Arch,Flatpak,RPM}/*`. **Install layouts stay flat** (`<root>/dashboard/ui/dist`) — only the *source* `cd` paths gain `code/frontend/`.
- Backend string mentions for human messaging (`contextAssembler.cpp` build-command hint, `workspaceIndexer.cpp` index path `workflow-editor/ui/src`) — retarget to `code/frontend/...`.
- **Verify:** launch Studio, load dashboard + `/editor`, confirm both render (proves the resolver + builds).

### Slice 5 — mcp → `code/mcp/`
- `git mv mcp code/mcp`. Update doc/script references (`mcp/README.md` link targets, build instructions, `.mcp.json.example` if added later).
- **Verify:** `cd code/mcp && npm install && npm run build` → `dist/` present; MCP tools reachable.

### Slice 6 — vendor → `code/vendor/` (heaviest, most build-critical)
- `git mv vendor code/vendor`.
- `premake5.lua` edits **only** (vendor scripts self-relative, untouched):
  - `files`: the 3 explicit entries (`vendor/simdjson/simdjson.{cpp,h}`, `vendor/date/src/tz.cpp`) → `code/vendor/...`.
  - `includedirs`: ~12 `vendor/...` entries → `code/vendor/...`.
  - `include "vendor/foo.lua"` × 6 → `include "code/vendor/foo.lua"`.
  - `_ACTION == "clean"` block: ~10 `os.rmdir("vendor/...")` + `os.remove("vendor/...")` → `code/vendor/...`.
- `.gitignore`: the `vendor/curl/...`, `vendor/openssl/...`, `vendor/pdcursesmod/...` build-output lines → `code/vendor/...`.
- **Verify:** `premake5 clean` → full 4-config rebuild from scratch warming-free (this slice rebuilds all vendored libs).

### Slice 7 — Doc + script sweep + reference-integrity gate
- Sweep `CLAUDE.md`, `doc/*` (architecture, jarvisagent.md/.1/.html, api-endpoints, cyber security, etc.), `README.md`, `DEVELOPMENT.md`, `INSTALL.md`, `mcp/README.md`, `integration/README.md` for `application/`, `engine/`, `dashboard/`, `workflow-editor/`, `shared-ui`, `vendor/`, `mcp/`, `tools/` path mentions → new locations. Regenerate `jarvisagent.{1,html}` via the documented pandoc step (don't hand-edit the generated `.html`).
- Audit `scripts/` build helpers (`compile.sh`, `link.sh`, `premake.sh`, `runMake.sh`, `run.sh`) for hardcoded `application/`/`engine/`/`vendor/` paths.
- Skip the generated `combined*.md` / audit outputs (regenerated from JCWFs, never hand-edited).
- Run the reference-integrity checker (below) until it reports zero stragglers.

---

## Reference-integrity verification (JC's "Python analyzer" idea)

A `scripts/check_path_refs.py` (added in Slice 1, removed or kept as a dev tool at the end — JC's call) that:
1. Walks all **git-tracked** text files (skips `code/vendor/`, `node_modules/`, generated `combined*.md`, binary blobs).
2. Greps for literal occurrences of the **old** top-level path prefixes that should no longer exist: `application/`, `engine/`, `dashboard/`, `workflow-editor/`, `shared-ui`, `mcp/`, `tools/`, root `Dockerfile`/`docker-*`, `DEVELOPMENT.md`/`INSTALL.md`/`todo.md` at root.
3. Allowlists legitimate survivors (e.g. `code/backend/application/...` is fine; the goal is to catch a *bare* `application/` that wasn't retargeted, and references to files that moved).
4. Prints `file:line` for every straggler and exits non-zero if any remain.

Run after each source-move slice, not just at the end — it localizes a miss to the slice that introduced it. This is the "double/triple check" gate JC asked for; it's mechanical and falsifiable.

**Final gate (the real proof):** `premake5 clean` → engine D/R + studio D/R warning-free; dashboard + editor + `code/mcp` rebuilt; launch + load both UIs; live test sweep (auth_mcp, negative_paths, keymanager_caps, s3_roundtrip, url-policy, slug, malformed-configs, SigV4 KAT, heap-scan). Same battery as the Part-7 closeout.

---

## Disposition of flagged files

| File / dir | Disposition | Why |
|---|---|---|
| `queue/ workflows/ log/ assistant/ .npm-tools/` | untrack, gitignore whole dir | runtime scratch; created on demand; stop showing empty on GitHub |
| `.npm-tools/package.json` | delete | stub; launcher installs its own; dir is a runtime `mmdc` target |
| `jarvis_agent.example.env` | delete | stale — env-credential path purged; only a dev-local Tracy flag left |
| `application/workflow/doc/aiCallArchitecture.md` | delete | superseded by AI-dispatch refactor; describes removed flow |
| `tools/{clean,print_docs,print_files,test}.sh` | delete | unused (per JC) |
| `tools/clang-format.sh` | move → `scripts/`, retarget find | the one keeper in `tools/` |
| Docker cluster (3 files) | move → `packaging/Docker/` | declutter root |
| `.dockerignore` | **stays at root** | Docker reads it only from build-context root |
| `.clangd` `.editorconfig` | **stay at root** (recommend keep tracked) | shared dev config; `.editorconfig` needs root for `root=true`; root `.clangd` covers the whole tree incl. `code/backend/` |
| `DEVELOPMENT.md INSTALL.md` | move → `doc/` | long-form docs out of root |
| `todo.md` | **stays at root** | live-TODO convention; JC's call |
| `compile_commands.json` | already untracked | generated; no action |
| **No `aux/` folder** | not needed | docker→packaging, docs→doc/, env file deleted — nothing left to park |

---

## Decisions (resolved with JC)

1. **Static-serve strategy** — ordered-root resolver (Slice 4). Install layouts stay flat; dev tree resolves under `code/frontend/`.
2. **Vendor move** — in scope, done last (Slice 6) once backend/frontend/mcp are proven green.
3. **Live TODO location** — `todo.md` stays at root (live-TODO convention unchanged); only `DEVELOPMENT.md` + `INSTALL.md` move to `doc/`.
4. **Commit shape** — one big reorg commit. Slices are an execution/verification discipline only; JC makes the single commit at the end.
```
