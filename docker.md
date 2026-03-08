# Docker Workflow — Design & Implementation Plan

## Current State

### What works
- `docker pull ghcr.io/beaumanvienna/jarvisagent:main` pulls the latest image.
- `docker run -it --rm -p 8080:8080 ghcr.io/beaumanvienna/jarvisagent:main` starts JarvisAgent.
- The **ncurses terminal UI** renders because `-it` allocates a TTY.
- The **React dashboard** is served at `http://localhost:8080/` (built in a dedicated `dashboard-builder` stage).
- Python tooling (`markitdown`, `md2pdf-mermaid`) and Chrome are pre-installed.

### What is broken / missing
| Issue | Root Cause |
|-------|-----------|
| `/editor` returns 404 | The Dockerfile does not build or copy `workflow-editor/ui/dist`. |
| No way to get files in/out | No volume mount guidance and no upload/download REST API. |
| No `docker run` docs in README | Only a table row mentions the GHCR image; no usage instructions. |
| API keys must be passed every time | No guidance on env vars or `.env` file for `OPENAI_API_KEY` / `JARVIS_MASTER_PASSWORD`. |
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

### 1.2 Document `docker run` in README.md

Add a **Docker** subsection under "Pre-built Packages" or a dedicated top-level section:

```markdown
### Docker

```bash
docker pull ghcr.io/beaumanvienna/jarvisagent:latest

docker run -it --rm \
  -p 8080:8080 \
  -e OPENAI_API_KEY=sk-... \
  -v ~/jarvis-data:/app/data \
  ghcr.io/beaumanvienna/jarvisagent:latest
```

- Dashboard: http://localhost:8080
- Workflow Editor: http://localhost:8080/editor
- The `-v` flag mounts a host directory for persistent workflows and outputs.
```

### 1.3 Provide a standard volume mount layout

Inside the container, restructure (or symlink) so that a single mount point
exposes everything the user cares about:

```
/app/data/               ← mount point (-v ~/jarvis-data:/app/data)
  ├── workflows/         ← .jcwf files (user-created and examples)
  ├── queue/             ← AI call inputs & outputs
  ├── config.json        ← runtime config (persisted across restarts)
  └── log/               ← log files
```

The user can simply browse `~/jarvis-data/` on the host to see all JCWF files,
input files, and output artifacts without any API or upload/download mechanism.

**This is the simplest, most effective, and safest approach for Phase 1:**
- No new code required (just Dockerfile + docs).
- Users get full read/write access to their files on the host.
- Works with any editor, file manager, or script on the host side.

---

## Phase 2 — Browser-Based File Transfer (medium effort, great UX)

For users who cannot or do not want to use volume mounts (e.g., remote Docker
hosts, cloud VMs, Docker Desktop on Windows), JarvisAgent should offer
upload/download directly through the browser.

### 2.1 REST API endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET`  | `/api/files?path=workflows/` | List files in a directory (relative to workdir). |
| `GET`  | `/api/files/download?path=workflows/example.jcwf` | Download a single file. |
| `POST` | `/api/files/upload?path=workflows/` | Upload one or more files (multipart). |
| `GET`  | `/api/files/download-zip?path=queue/exampleMakefile/` | Download a directory as `.zip`. |
| `DELETE` | `/api/files?path=workflows/old.jcwf` | Delete a file (with confirmation in UI). |

**Security considerations:**
- Path traversal protection: reject any path containing `..` or starting with `/`.
- Restrict to the `/app/data/` subtree only.
- Optional: require `JARVIS_MASTER_PASSWORD` for destructive operations.

### 2.2 Dashboard File Manager panel

A lightweight file browser in the React dashboard:
- Tree view of `workflows/`, `queue/`, `log/`.
- Drag-and-drop upload zone for `.jcwf` files, input documents, etc.
- One-click download for individual files or entire directories (as `.zip`).
- "Import JCWF" button in the Workflow Editor that opens the upload dialog.

### 2.3 Workflow Editor integration

- **Export:** "Download .jcwf" button already exists (saves to disk via browser).
  Works as-is for Docker — the file downloads to the user's host machine.
- **Import:** Add an "Upload .jcwf" button that POSTs to `/api/files/upload`
  and then reloads the workflow registry.

---

## Phase 3 — Docker Compose & Production Hardening (lower priority)

### 3.1 docker-compose.yml

```yaml
services:
  jarvisagent:
    image: ghcr.io/beaumanvienna/jarvisagent:latest
    ports:
      - "8080:8080"
    environment:
      - OPENAI_API_KEY=${OPENAI_API_KEY}
    volumes:
      - jarvis-data:/app/data
    restart: unless-stopped

volumes:
  jarvis-data:
```

### 3.2 Multi-arch support

The current CI builds `linux/amd64` only. Add `linux/arm64` for Apple Silicon
Docker Desktop users and ARM cloud instances.

### 3.3 Smaller image

- Consider Alpine or distroless base to reduce image size.
- Multi-stage pruning of build artifacts.
- Chrome is ~400 MB; consider making it optional or using a sidecar.

---

## Implementation Priority

| Priority | Item | Effort | Impact |
|----------|------|--------|--------|
| **P0** | Bundle workflow editor in Docker image (1.1) | 15 min | `/editor` works |
| **P0** | Document `docker run` in README (1.2) | 15 min | Users can actually run it |
| **P1** | Volume mount layout + docs (1.3) | 30 min | Files persist, host access |
| **P2** | REST file transfer API (2.1) | 2–3 hrs | Remote Docker support |
| **P2** | Dashboard File Manager UI (2.2) | 3–4 hrs | Browser-based file I/O |
| **P3** | docker-compose.yml (3.1) | 15 min | One-command deployment |
| **P3** | Multi-arch build (3.2) | 30 min | ARM / Apple Silicon |
| **P3** | Image size optimization (3.3) | 1–2 hrs | Faster pulls |

---

## Open Questions

1. Should the volume mount point be `/app/data` (clean, new) or should we mount
   `/app/queue`, `/app/workflows`, `/app/log` separately (matches current layout)?
   **Recommendation:** Single mount at `/app/data` with symlinks from the legacy
   paths. Simpler for the user, one `-v` flag.

2. Should the file transfer API (Phase 2) be Docker-only or always available?
   **Recommendation:** Always available — it's useful for remote/SSH deployments
   too, not just Docker.

3. Should we add a "first-run wizard" in the dashboard that detects missing API
   keys and guides the user through configuration?
   **Recommendation:** Yes, especially for Docker users who may not know about
   env vars.
