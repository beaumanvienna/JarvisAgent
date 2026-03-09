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
    zlib1g \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# markitdown for PDF-to-markdown conversion
RUN pipx install "markitdown[all]"

# md2pdf-mermaid for markdown-to-PDF conversion
RUN pipx install md2pdf-mermaid

# Google Chrome for Playwright (used by md2pdf-mermaid)
RUN apt-get update && apt-get install -y --no-install-recommends \
    wget gnupg && \
    wget -q -O - https://dl.google.com/linux/linux_signing_key.pub | gpg --dearmor -o /usr/share/keyrings/google-chrome.gpg && \
    echo "deb [arch=amd64 signed-by=/usr/share/keyrings/google-chrome.gpg] https://dl.google.com/linux/chrome/deb/ stable main" > /etc/apt/sources.list.d/google-chrome.list && \
    apt-get update && apt-get install -y --no-install-recommends google-chrome-stable && \
    rm -rf /var/lib/apt/lists/*

ENV PATH="/root/.local/bin:$PATH"
ENV CHROME_PATH="/usr/bin/google-chrome"

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
     example/workflows/exampleMakefile4.jcwf \
     example/workflows/make-example.jcwf \
     example/workflows/portfolioDividendAnalysis.jcwf \
     example/workflows/vehicleTroubleshootingGuide.jcwf \
     /opt/jarvisagent/.image-defaults/workflows/
COPY example/workflows/app.cpp \
     example/workflows/lib1.cpp \
     example/workflows/lib2.cpp \
     example/workflows/main.cpp \
     example/workflows/mylib.h \
     example/workflows/message_engine_question.txt \
     example/workflows/message_tire_question.txt \
     example/workflows/message_unclear_question.txt \
     example/workflows/port62pos.csv \
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
