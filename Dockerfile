# ---- Dashboard build stage ----
FROM node:20-slim AS dashboard-builder
WORKDIR /ui
COPY dashboard/ui/package.json dashboard/ui/package-lock.json ./
RUN npm ci
COPY dashboard/ui/ ./
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
    libssl-dev \
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
    libssl3t64 \
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

# Runtime assets
COPY --chown=appuser:appuser config.json /app/config.json
COPY --chown=appuser:appuser scripts /app/scripts
COPY --chown=appuser:appuser example/workflows /app/example/workflows
COPY --from=dashboard-builder /ui/dist /app/dashboard/ui/dist
RUN chown -R appuser:appuser /app/dashboard

RUN mkdir -p /app/queue /app/workflows /app/log && chown -R appuser:appuser /app

USER appuser

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD python3 -c "import urllib.request; urllib.request.urlopen('http://localhost:8080/api/status')" || exit 1

CMD ["./jarvisAgent"]
