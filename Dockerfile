# ---- Dashboard build stage ----
FROM node:20-slim AS dashboard-builder
WORKDIR /ui
COPY dashboard/ui/package.json dashboard/ui/package-lock.json ./
RUN npm ci
COPY dashboard/ui/ ./
RUN npm run build

# ---- Workflow Editor build stage ----
FROM node:20-slim AS editor-builder
WORKDIR /ui
COPY workflow-editor/ui/package.json workflow-editor/ui/package-lock.json ./
RUN npm ci
COPY workflow-editor/ui/ ./
RUN npm run build

# ---- Builder stage ----
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    pkg-config \
    wget \
    ca-certificates \
    python3 \
    python3-dev \
    zlib1g-dev \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

# premake5
RUN cd /tmp && \
    wget -q https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-linux.tar.gz && \
    tar -xzf premake-5.0.0-beta2-linux.tar.gz && \
    mv premake5 /usr/local/bin/ && \
    chmod +x /usr/local/bin/premake5 && \
    rm premake-5.0.0-beta2-linux.tar.gz

WORKDIR /app
COPY . .

# Build Engine edition
RUN premake5 gmake --engine && make -j$(nproc) config=release verbose=1

# Build Studio edition (default)
RUN premake5 gmake && make -j$(nproc) config=release verbose=1

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

# Curated example workflows for first-run seeding.
# All input files are bundled inside each .jcwf zip — no loose files to copy.
RUN mkdir -p /opt/jarvisagent/.image-defaults/workflows
COPY example/workflows/aiCarMaintenancePipeline.jcwf \
     example/workflows/aiZipDemo.jcwf \
     example/workflows/make-example.jcwf \
     example/workflows/make-example-subwf.jcwf \
     example/workflows/portfolioDividendAnalysis.jcwf \
     example/workflows/vehicleTroubleshootingGuide.jcwf \
     example/workflows/bookSummaryPipeline.jcwf \
     example/workflows/hamburg-tourist-day-planner.jcwf \
     example/workflows/exampleMakefile4.jcwf \
     example/workflows/exampleMakefile5.jcwf \
     example/workflows/goKartComplianceCheck.jcwf \
     example/workflows/s3UploadDownloadDemo.jcwf \
     example/workflows/postgresDemo.jcwf \
     example/workflows/emailDemo.jcwf \
     example/workflows/gitHubIssueDemo.jcwf \
     example/workflows/azureBlobDemo.jcwf \
     example/workflows/gcsDemo.jcwf \
     /opt/jarvisagent/.image-defaults/workflows/

# Default config for first-run seeding
COPY config.json /opt/jarvisagent/.image-defaults/config.json

# Image version marker for upgrade detection (entrypoint re-seeds workflows on version change)
RUN echo "0.8.5" > /opt/jarvisagent/.image-defaults/.image-version

# Entrypoint script
COPY docker-entrypoint.sh /opt/jarvisagent/docker-entrypoint.sh
RUN chmod +x /opt/jarvisagent/docker-entrypoint.sh

# /app is the data directory (volume mount point)
WORKDIR /app

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD python3 -c "import urllib.request; urllib.request.urlopen('http://localhost:8080/api/status')" || exit 1

ENTRYPOINT ["/opt/jarvisagent/docker-entrypoint.sh"]
