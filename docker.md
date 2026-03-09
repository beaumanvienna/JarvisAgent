# Docker Workflow — Design & Implementation Plan

## Current State

### What works
- `docker pull ghcr.io/beaumanvienna/jarvisagent:main` pulls the latest image.
- `docker run -it --rm -p 8080:8080 ghcr.io/beaumanvienna/jarvisagent:main` starts JarvisAgent.
- The **ncurses terminal UI** renders because `-it` allocates a TTY.
- The **React dashboard** is served at `http://localhost:8080/` (built in a dedicated `dashboard-builder` stage).
- Python tooling (`markitdown`, `md2pdf-mermaid`) and Chrome are pre-installed.
- CI pipeline (`.github/workflows/docker-publish.yml`) builds and pushes the image to `ghcr.io` on every push to main/master/develop with multi-tag support (`latest`, `main`, `main-<sha>`).

### What is broken / missing
| Issue | Root Cause |
|-------|-----------|
| `/editor` returns 404 | The Dockerfile does not build or copy `workflow-editor/ui/dist`. |
| No example workflows shipped | Dockerfile copies `example/workflows` to `/app/example/workflows` but `workflows/` is empty. Should ship the same curated set as DEB/RPM/Arch packages. |
| No way to get files in/out | No volume mount guidance and no upload/download REST API. |
| No `docker run` docs in README | Only a table row mentions the GHCR image; no usage instructions. |
| API keys lost on container restart | No volume mount for `keys.json.enc`. Users who configure keys via the workflow editor UI lose them on `docker rm`. |
| Dashboard has no master password prompt | The workflow editor already prompts via `MasterPasswordDialog` on page load when `keys.json.enc` exists but is locked. The dashboard does not — it only shows a "no keys" banner. Both UIs should prompt. |
| Workflows are ephemeral | Container filesystem is lost on `docker rm`; user work disappears. |

---

## Phase 1 — Fix the Image (low effort, high impact)

### 1.1 Bundle the Workflow Editor in the Docker image

Add a second Node build stage to the Dockerfile:

```dockerfile
# ---- Workflow Editor build stage ----
FROM node:20-slim AS editor-builder
WORKDIR /ui
COPY workflow-editor/ui/package.json workflow-editor/ui/package-lock.json ./
RUN npm ci
COPY workflow-editor/ui/ ./
RUN npm run build
```

Then in the runtime stage, add:

```dockerfile
COPY --from=editor-builder /ui/dist /app/workflow-editor/ui/dist
RUN chown -R appuser:appuser /app/workflow-editor
```

**Result:** `/editor` works out of the box.

### 1.2 Ship curated example workflows

Ship the same set of example workflows that DEB/RPM/Arch packages include
(see `packaging/Linux/Arch/PKGBUILD` `package()` for the canonical list):

**JCWF files (6):**
- `aiCarMaintenancePipeline.jcwf`
- `aiZipDemo.jcwf`
- `exampleMakefile4.jcwf`
- `make-example.jcwf`
- `portfolioDividendAnalysis.jcwf`
- `vehicleTroubleshootingGuide.jcwf`

**Auxiliary input files needed by those workflows:**
- `app.cpp`, `lib1.cpp`, `lib2.cpp`, `main.cpp`, `mylib.h` (make-example / exampleMakefile4)
- `message_engine_question.txt`, `message_tire_question.txt`, `message_unclear_question.txt` (vehicleTroubleshootingGuide / aiCarMaintenancePipeline)
- `port62pos.csv` (portfolioDividendAnalysis)
- `message.txt` → symlink to `message_engine_question.txt` (aiCarMaintenancePipeline default input)

Add to the Dockerfile runtime stage:

```dockerfile
# ---- Example workflows (same curated set as DEB/RPM/Arch packages) ----
COPY --chown=appuser:appuser example/workflows/aiCarMaintenancePipeline.jcwf /app/workflows/
COPY --chown=appuser:appuser example/workflows/aiZipDemo.jcwf /app/workflows/
COPY --chown=appuser:appuser example/workflows/exampleMakefile4.jcwf /app/workflows/
COPY --chown=appuser:appuser example/workflows/make-example.jcwf /app/workflows/
COPY --chown=appuser:appuser example/workflows/portfolioDividendAnalysis.jcwf /app/workflows/
COPY --chown=appuser:appuser example/workflows/vehicleTroubleshootingGuide.jcwf /app/workflows/
COPY --chown=appuser:appuser example/workflows/app.cpp example/workflows/lib1.cpp \
     example/workflows/lib2.cpp example/workflows/main.cpp example/workflows/mylib.h \
     example/workflows/message_engine_question.txt example/workflows/message_tire_question.txt \
     example/workflows/message_unclear_question.txt example/workflows/port62pos.csv \
     /app/workflows/
RUN ln -sf message_engine_question.txt /app/workflows/message.txt
```

**Result:** Users see working example workflows immediately after `docker run`. The
`make-example` and `exampleMakefile4` workflows work without any AI key (shell tasks
only) — perfect for a first test.

### 1.3 Volume mount at `~/JarvisAgent`

The host-side directory is `~/JarvisAgent` — the same name used by the Linux
launcher (`packaging/Linux/jarvisagent-launcher.sh`) and all native packages.
Docker's `-v` flag maps this host directory into the container at `/app`
(the container's CWD where j9t resolves all relative paths).

```bash
docker run -it --rm \
  -p 8080:8080 \
  -v ~/JarvisAgent:/app \
  ghcr.io/beaumanvienna/jarvisagent:latest
```

**How it works:** `~/JarvisAgent` is a directory on the **host machine** (the
user's computer). Docker mounts it into the container at `/app` so that the
container process reads/writes to the host filesystem. The container itself has
no direct access to `~/` — it only sees `/app`, which is backed by the host
directory.

**Container layout — read-only assets vs. user data:**

Read-only image assets (binary, React UIs, scripts) live in `/opt/jarvisagent/`
inside the container. They are **not** affected by the volume mount. The
entrypoint script creates symlinks from `/app` into `/opt/jarvisagent/` so the
binary finds everything relative to its CWD (`/app`).

```
/opt/jarvisagent/                ← read-only image assets (survive volume mount)
  ├── jarvisAgent                ← binary
  ├── docker-entrypoint.sh       ← entrypoint script
  ├── dashboard/ui/dist/         ← dashboard React build
  ├── workflow-editor/ui/dist/   ← editor React build
  ├── scripts/                   ← shell/Python scripts
  └── .image-defaults/           ← example workflows + config for first-run seeding

/app/                            ← WORKDIR, volume mount point (~/JarvisAgent on host)
  ├── workflows/                 ← .jcwf files (user-created and seeded examples)
  ├── queue/                     ← AI call inputs & outputs
  ├── config.json                ← runtime config (persisted)
  ├── keys.json.enc              ← encrypted API keys (persisted)
  ├── log/                       ← log files
  ├── dashboard → /opt/...       ← symlink (created by entrypoint)
  ├── workflow-editor → /opt/... ← symlink (created by entrypoint)
  └── scripts → /opt/...         ← symlink (created by entrypoint)
```

**First-run seeding (mirrors `jarvisagent-launcher.sh` behavior):**

The Docker entrypoint script (`docker-entrypoint.sh`) runs on every start:
1. Creates symlinks: `dashboard`, `workflow-editor`, `scripts` → `/opt/jarvisagent/...`
2. If `workflows/` is empty or does not exist → copies the curated example
   workflows from the image defaults into the mounted volume.
3. If `config.json` does not exist → copies the default config from the image.
4. Creates `queue/` and `log/` if missing.

Subsequent runs leave user files untouched — only seed missing assets.
Symlinks are re-created every start (cheap and idempotent).

This is the same logic that `packaging/Linux/jarvisagent-launcher.sh` uses
for native DEB/RPM/Arch installs (copy examples on first run, skip if directory
is non-empty).

### 1.4 Document `docker run` in README.md

Add a **Docker** subsection under "Pre-built Packages" or a dedicated top-level section:

````markdown
### Docker

```bash
docker pull ghcr.io/beaumanvienna/jarvisagent:latest

docker run -it --rm \
  -p 8080:8080 \
  -v ~/JarvisAgent:/app \
  ghcr.io/beaumanvienna/jarvisagent:latest
```

- Dashboard: http://localhost:8080
- Workflow Editor: http://localhost:8080/editor
- The `-v` flag mounts `~/JarvisAgent` on the host so workflows, AI keys, and outputs persist across container restarts.
- To configure AI providers, open the Workflow Editor → AI Keys page.
````

---

## Phase 2 — Key Management for Docker (medium effort, high impact)

### Motivation

Docker users need persistent, multi-provider AI key management without
complicated CLI env vars. The workflow editor already has a complete key
management UI (AI Manager + AI Keys views) and a `MasterPasswordDialog` that
prompts automatically. The approach is: **persist the key file via the volume
mount and let the browser UI handle key setup**.

### 2.1 Key file persistence via volume mount

`keys.json.enc` is written to the CWD (`/app`) by the backend when the user
clicks "Save Encrypted" in the workflow editor's AI Keys view. With the volume
mount at `~/JarvisAgent:/app`, the encrypted file persists at
`~/JarvisAgent/keys.json.enc` on the host.

**User workflow:**
1. `docker run -it --rm -p 8080:8080 -v ~/JarvisAgent:/app ghcr.io/beaumanvienna/jarvisagent:latest`
2. Open http://localhost:8080/editor → AI Keys tab.
3. Add providers (OpenAI, Anthropic, Ollama, etc.) with their API keys.
4. Click "Save Encrypted" → enter a master password → `keys.json.enc` is
   written to `~/JarvisAgent/keys.json.enc` on the host.
5. On subsequent runs, the workflow editor auto-prompts for the master password
   on page load (via `MasterPasswordDialog`).

**No additional env vars needed** for multi-provider setups — all managed
through the browser UI.

### 2.2 Add master password prompt to the Dashboard

**Current state:**
- **Workflow editor** (`/editor`): Has `MasterPasswordDialog` in `App.tsx`
  that checks `/api/settings/keys/status` on mount and prompts if status is
  `"no_password"` or `"wrong_password"`. Works well.
- **Dashboard** (`/`): Has no master password prompt — only shows a static
  "No AI providers configured" banner in `WorkflowsPanel.tsx` (lines 66–71).

**Required change:** Add the same `MasterPasswordDialog` flow to the dashboard
so that Docker users who open the dashboard first (not the editor) are prompted
to unlock their keys. The dashboard already calls `usePolling` which returns
`hasProviders` — we need to also check `/api/settings/keys/status` and show
the dialog when keys exist but are locked.

**Implementation:**
1. Copy or share `MasterPasswordDialog` component into `dashboard/ui/src/components/`.
2. Copy or share `api/keys.ts` (`getKeysStatus`, `unlockKeys`) into `dashboard/ui/src/api/`.
3. In `dashboard/ui/src/App.tsx`, add the same `useEffect` + state logic as the
   workflow editor's `App.tsx` (lines 50–60, 263–272):
   - On mount, call `getKeysStatus()`.
   - If status is `"no_password"` or `"wrong_password"`, render `MasterPasswordDialog`.
   - On successful unlock, dismiss the dialog and refresh providers.

### 2.3 Legacy `OPENAI_API_KEY` env var (backward compatibility)

The existing `OPENAI_API_KEY` environment variable continues to work as today:
if no `keys.json.enc` exists, JarvisAgent falls back to this env var and creates
a single `openai` provider. This provides a simple one-liner for users who only
need OpenAI:

```bash
docker run -it --rm -p 8080:8080 \
  -e OPENAI_API_KEY=sk-... \
  -v ~/JarvisAgent:/app \
  ghcr.io/beaumanvienna/jarvisagent:latest
```

**Resolution order:**
1. **`keys.json.enc`** + master password → full multi-provider support.
2. **`OPENAI_API_KEY`** env var → single OpenAI provider (legacy fallback).

### 2.4 docker-compose.yml

```yaml
services:
  jarvisagent:
    image: ghcr.io/beaumanvienna/jarvisagent:latest
    ports:
      - "8080:8080"
    volumes:
      - ~/JarvisAgent:/app
    restart: unless-stopped
    # Optional: legacy single-provider shortcut
    # environment:
    #   - OPENAI_API_KEY=${OPENAI_API_KEY}

volumes: {}
```

A production-ready `docker-compose.example.yml` with Traefik labels for
reverse-proxy setups already exists in the repo.

---

## Implementation Priority

| Priority | Item | Effort | Impact |
|----------|------|--------|--------|
| **P0** | Bundle workflow editor in Docker image (1.1) | 15 min | `/editor` works | ✅ Done |
| **P0** | Ship curated example workflows (1.2) | 15 min | Users have working examples on first run | ✅ Done |
| **P0** | Volume mount at `~/JarvisAgent` + entrypoint (1.3) | 30 min | Files + keys persist, host access | ✅ Done |
| **P0** | Document `docker run` in README (1.4) | 15 min | Users can actually run it | ✅ Done |
| **P1** | Add master password prompt to dashboard (2.2) | 1–2 hrs | Keys unlock from dashboard, not just editor | ✅ Done |

---

## Resolved Questions

1. **Volume mount path:** `~/JarvisAgent` on the host, mapped to `/app` in the
   container. Same directory name as native Linux/macOS installs. One `-v` flag.

2. **Folder structure:** Same layout as the Linux launcher
   (`packaging/Linux/jarvisagent-launcher.sh`): `workflows/`, `queue/`, `log/`,
   `scripts/`, `config.json`, `keys.json.enc`.

3. **Key persistence:** `keys.json.enc` lives in the volume mount. Users manage
   keys through the workflow editor's AI Keys view (browser UI). No complex
   env var scheme needed — just the volume mount. Legacy `OPENAI_API_KEY` env
   var kept for backward compatibility.

4. **Master password prompt:** Both the workflow editor and the dashboard now
   prompt via `MasterPasswordDialog` when `keys.json.enc` exists but is locked.

5. **Example workflows:** Ship the same curated set as DEB/RPM/Arch packages
   (6 JCWF + auxiliary files). Seeded into the volume on first run (same
   behavior as `jarvisagent-launcher.sh`).

6. **Browser-based file transfer:** Dropped. The volume mount at `~/JarvisAgent`
   gives all Docker users (Linux, macOS, Windows) direct filesystem access.
   Docker Desktop on Windows and macOS fully supports volume mounts. A REST
   file API can be added later if needed for remote/headless deployments.

7. **Apple Silicon / ARM64:** Not needed for Docker. macOS users install the
   native DMG package directly.
