# ---- Dashboard build stage ----
# Both apps import from ../../shared-ui via the @shared/* alias. shared-ui is
# source-only (no package.json / node_modules); tsconfig paths + Vite dedupe
# pin react/react-dom to the consumer's node_modules.
FROM node:20-slim AS dashboard-builder
WORKDIR /ui
COPY code/frontend/shared-ui/ /shared-ui/
COPY code/frontend/dashboard/ui/package.json code/frontend/dashboard/ui/package-lock.json ./
RUN npm ci
COPY code/frontend/dashboard/ui/ ./
RUN npm run build

# ---- Workflow Editor build stage ----
FROM node:20-slim AS editor-builder
WORKDIR /ui
COPY code/frontend/shared-ui/ /shared-ui/
COPY code/frontend/workflow-editor/ui/package.json code/frontend/workflow-editor/ui/package-lock.json ./
RUN npm ci
COPY code/frontend/workflow-editor/ui/ ./
RUN npm run build

# ---- Builder stage ----
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    pkg-config \
    git \
    uuid-dev \
    ca-certificates \
    python3 \
    python3-dev \
    zlib1g-dev \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

# premake5 — built from source so the same recipe works on both linux/amd64
# and linux/arm64 (no official arm64 prebuilt in the v5.0.0-beta8 release).
RUN git clone --depth 1 --branch v5.0.0-beta8 https://github.com/premake/premake-core.git /tmp/premake-core && \
    cd /tmp/premake-core && \
    make -f Bootstrap.mak linux && \
    cp bin/release/premake5 /usr/local/bin/ && \
    chmod +x /usr/local/bin/premake5 && \
    rm -rf /tmp/premake-core

WORKDIR /app

# --- Vendor static-library layer (cached across runs) ---
# Copy ONLY the inputs needed to build the third-party static libs so this
# layer's cache key is independent of application source changes. Touching a
# .cpp under application/ no longer invalidates this 20+ min OpenSSL compile.
COPY premake5.lua ./
COPY code/vendor/ ./code/vendor/

RUN premake5 gmake && \
    make -j$(nproc) config=release \
        crypto ssl curl nghttp2 pdcursesmod miniz

# --- Application layer ---
# vendor/ mtimes are preserved (we don't re-copy it), so the vendor .a files
# stay "up-to-date" from make's perspective and are not rebuilt.
# doc/ is needed at premake-generation time: embedAsHeader reads
# doc/jcwf.schema.json + doc/jcwf_generation_guide.md and writes the generated
# headers that aiJcwfService.cpp (Studio) and the validator (both editions) include.
COPY doc/ ./doc/
COPY code/backend/application/ ./code/backend/application/
COPY code/backend/engine/ ./code/backend/engine/

# Build Engine edition (re-runs premake with --engine to switch jarvisAgent
# objdir to bin-int/engine/; vendor projects are unchanged and skipped).
RUN premake5 gmake --engine && \
    make -j$(nproc) config=release verbose=1 jarvisAgent

# Build Studio edition (default)
RUN premake5 gmake && \
    make -j$(nproc) config=release verbose=1 jarvisAgent

# ---- Runtime stage ----
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-dev \
    python3-pip \
    python3-venv \
    pipx \
    gosu \
    zlib1g \
    libpq5 \
    ca-certificates \
    # Node.js + mmdc (Mermaid → PNG rendering)
    nodejs \
    npm \
    # pandoc + pdflatex (Markdown → PDF)
    pandoc \
    texlive-latex-base \
    texlive-latex-extra \
    texlive-fonts-recommended \
    # Chromium system deps required by mmdc's bundled Puppeteer
    libnss3 \
    libatk1.0-0 \
    libatk-bridge2.0-0 \
    libcups2 \
    libdrm2 \
    libxkbcommon0 \
    libxcomposite1 \
    libxdamage1 \
    libxfixes3 \
    libxrandr2 \
    libgbm1 \
    libasound2t64 \
    fonts-liberation \
    && rm -rf /var/lib/apt/lists/*

# Install pipx tools into a shared location accessible by any UID (gosu drops to host user)
ENV PIPX_HOME=/opt/pipx
ENV PIPX_BIN_DIR=/opt/pipx/bin

# markitdown for document-to-markdown conversion
RUN pipx install "markitdown[all]"

# mmdc (mermaid-cli) for Mermaid diagram → PNG rendering
# Pinned to 10.x — stable, monthly releases, no Playwright dependency
RUN npm install -g @mermaid-js/mermaid-cli@10.x

# Puppeteer config: disable sandbox (required in Docker without user namespaces)
RUN echo '{"args":["--no-sandbox","--disable-setuid-sandbox"]}' \
    > /etc/mmdc-puppeteer-config.json

RUN chmod -R a+rX /opt/pipx

ENV PATH="/opt/pipx/bin:$PATH"

# ---- Read-only image assets in /opt/jarvisagent/ ----
# These survive the volume mount at /app. The entrypoint creates symlinks
# from /app back to these so the binary finds them relative to CWD.

# Binaries (Studio + Engine)
COPY --from=builder /app/bin/Release/jarvisAgent-studio /opt/jarvisagent/jarvisAgent-studio
COPY --from=builder /app/bin/Release/jarvisAgent-engine /opt/jarvisagent/jarvisAgent-engine

# React UIs
COPY --from=dashboard-builder /ui/dist /opt/jarvisagent/dashboard/ui/dist
COPY --from=editor-builder /ui/dist /opt/jarvisagent/workflow-editor/ui/dist

# Scripts
COPY scripts /opt/jarvisagent/scripts

# Curated example workflows for first-run seeding — the SAME set every package
# ships.  Dockerfile COPY can't read a file, so these rows are kept in sync by
# hand with the single source of truth at packaging/curated-workflows.txt.
# (All input files are bundled inside each .jcwf zip — no loose files to copy.)
RUN mkdir -p /opt/jarvisagent/.image-defaults/workflows
COPY example/workflows/aiCarMaintenancePipeline.jcwf \
     example/workflows/aiZipDemo.jcwf \
     example/workflows/make-example.jcwf \
     example/workflows/portfolioDividendAnalysis.jcwf \
     example/workflows/vehicleTroubleshootingGuide.jcwf \
     /opt/jarvisagent/.image-defaults/workflows/

# Default config for first-run seeding.
# Uses the tracked canonical template (HTTP on port 0 → 8080, no TLS) rather
# than the developer's local config.json (which is gitignored and may point at
# TLS certs that aren't in the image). The entrypoint injects TLS fields on
# first-run seed when J9T_TLS=1.
COPY packaging/config.json.example /opt/jarvisagent/.image-defaults/config.json

# Image version marker for upgrade detection (entrypoint re-seeds workflows on version change)
RUN echo "0.8.8" > /opt/jarvisagent/.image-defaults/.image-version

# Entrypoint script
COPY packaging/Docker/docker-entrypoint.sh /opt/jarvisagent/docker-entrypoint.sh
RUN chmod +x /opt/jarvisagent/docker-entrypoint.sh

# /app is the data directory (volume mount point)
WORKDIR /app

EXPOSE 8080 8443

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD if [ "${J9T_TLS:-0}" = "1" ]; then \
            python3 -c "import urllib.request, ssl; urllib.request.urlopen('https://localhost:8443/api/status', context=ssl._create_unverified_context())"; \
        else \
            python3 -c "import urllib.request; urllib.request.urlopen('http://localhost:8080/api/status')"; \
        fi

ENTRYPOINT ["/opt/jarvisagent/docker-entrypoint.sh"]
