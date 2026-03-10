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
    wget \
    ca-certificates \
    python3 \
    python3-dev \
    zlib1g-dev \
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

RUN premake5 gmake
RUN make -j$(nproc) config=release verbose=1

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
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install pipx tools into a shared location accessible by any UID (gosu drops to host user)
ENV PIPX_HOME=/opt/pipx
ENV PIPX_BIN_DIR=/opt/pipx/bin

# markitdown for PDF-to-markdown conversion
RUN pipx install "markitdown[all]"

# md2pdf-mermaid for markdown-to-PDF conversion
RUN pipx install md2pdf-mermaid

# Install Playwright's Chromium browser + system dependencies for md2pdf-mermaid's PDF rendering
ENV PLAYWRIGHT_BROWSERS_PATH=/opt/playwright-browsers
RUN /opt/pipx/venvs/md2pdf-mermaid/bin/playwright install --with-deps chromium
RUN chmod -R a+rX /opt/pipx /opt/playwright-browsers

ENV PATH="/opt/pipx/bin:$PATH"

# ---- Read-only image assets in /opt/jarvisagent/ ----
# These survive the volume mount at /app. The entrypoint creates symlinks
# from /app back to these so the binary finds them relative to CWD.

# Binary
COPY --from=builder /app/bin/Release/jarvisAgent /opt/jarvisagent/jarvisAgent

# React UIs
COPY --from=dashboard-builder /ui/dist /opt/jarvisagent/dashboard/ui/dist
COPY --from=editor-builder /ui/dist /opt/jarvisagent/workflow-editor/ui/dist

# Scripts
COPY scripts /opt/jarvisagent/scripts

# Curated example workflows for first-run seeding (same set as DEB/RPM/Arch — see PKGBUILD)
RUN mkdir -p /opt/jarvisagent/.image-defaults/workflows
COPY example/workflows/aiCarMaintenancePipeline.jcwf \
     example/workflows/aiZipDemo.jcwf \
     example/workflows/make-example.jcwf \
     example/workflows/portfolioDividendAnalysis.jcwf \
     example/workflows/vehicleTroubleshootingGuide.jcwf \
     /opt/jarvisagent/.image-defaults/workflows/
COPY example/workflows/message_engine_question.txt \
     example/workflows/message_tire_question.txt \
     example/workflows/message_unclear_question.txt \
     example/workflows/port62pos.csv \
     example/workflows/app.cpp \
     example/workflows/lib1.cpp \
     example/workflows/lib2.cpp \
     example/workflows/main.cpp \
     example/workflows/mylib.h \
     /opt/jarvisagent/.image-defaults/workflows/
RUN ln -sf message_engine_question.txt /opt/jarvisagent/.image-defaults/workflows/message.txt

# Default config for first-run seeding
COPY config.json /opt/jarvisagent/.image-defaults/config.json

# Entrypoint script
COPY docker-entrypoint.sh /opt/jarvisagent/docker-entrypoint.sh
RUN chmod +x /opt/jarvisagent/docker-entrypoint.sh

# /app is the data directory (volume mount point)
WORKDIR /app

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD python3 -c "import urllib.request; urllib.request.urlopen('http://localhost:8080/api/status')" || exit 1

ENTRYPOINT ["/opt/jarvisagent/docker-entrypoint.sh"]
