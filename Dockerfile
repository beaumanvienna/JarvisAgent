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

RUN useradd -m -u 1001 -s /bin/bash appuser

WORKDIR /app

# Binary from builder
COPY --from=builder /app/bin/Release/jarvisAgent /app/jarvisAgent

# React UIs
COPY --from=dashboard-builder /ui/dist /app/dashboard/ui/dist
COPY --from=editor-builder /ui/dist /app/workflow-editor/ui/dist

# Scripts (read-only, always from the image)
COPY scripts /app/scripts

# ---- Image defaults for first-run seeding (mirrors jarvisagent-launcher.sh) ----
# These are copied into the volume on first run by docker-entrypoint.sh.
RUN mkdir -p /app/.image-defaults/workflows

# Curated example workflows (same set as DEB/RPM/Arch packages — see PKGBUILD)
COPY example/workflows/aiCarMaintenancePipeline.jcwf \
     example/workflows/aiZipDemo.jcwf \
     example/workflows/exampleMakefile4.jcwf \
     example/workflows/make-example.jcwf \
     example/workflows/portfolioDividendAnalysis.jcwf \
     example/workflows/vehicleTroubleshootingGuide.jcwf \
     /app/.image-defaults/workflows/
COPY example/workflows/app.cpp \
     example/workflows/lib1.cpp \
     example/workflows/lib2.cpp \
     example/workflows/main.cpp \
     example/workflows/mylib.h \
     example/workflows/message_engine_question.txt \
     example/workflows/message_tire_question.txt \
     example/workflows/message_unclear_question.txt \
     example/workflows/port62pos.csv \
     /app/.image-defaults/workflows/
RUN ln -sf message_engine_question.txt /app/.image-defaults/workflows/message.txt

# Default config
COPY config.json /app/.image-defaults/config.json

# Also keep scripts in image-defaults so entrypoint can symlink them
RUN cp -a /app/scripts /app/.image-defaults/scripts

# Entrypoint script
COPY docker-entrypoint.sh /app/docker-entrypoint.sh
RUN chmod +x /app/docker-entrypoint.sh

# Create default writable directories and fix ownership
RUN mkdir -p /app/queue /app/workflows /app/log && chown -R appuser:appuser /app

USER appuser

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD python3 -c "import urllib.request; urllib.request.urlopen('http://localhost:8080/api/status')" || exit 1

ENTRYPOINT ["./docker-entrypoint.sh"]
