
FROM ubuntu:22.04 AS builder


ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    wget \
    ca-certificates \
    python3 \
    python3-pip \
    python3-dev \
    libncurses5 \
    libncurses5-dev \
    libncursesw5 \
    libncursesw5-dev \
    libssl-dev \
    zlib1g-dev \
    perl \
    && rm -rf /var/lib/apt/lists/*


RUN pip3 install "markitdown[all]"

# premake5
RUN cd /tmp && \
    wget https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-linux.tar.gz && \
    tar -xzf premake-5.0.0-beta2-linux.tar.gz && \
    mv premake5 /usr/local/bin/ && \
    chmod +x /usr/local/bin/premake5 && \
    rm premake-5.0.0-beta2-linux.tar.gz


WORKDIR /app

COPY . .


RUN premake5 gmake

RUN make config=release verbose=1 -j4 2>&1 || make config=release verbose=1 -j1

FROM ubuntu:22.04


ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    libncurses5 \
    libncursesw5 \
    libssl3 \
    zlib1g \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*


RUN pip3 install "markitdown[all]"


RUN useradd -m -u 1001 -s /bin/bash appuser


WORKDIR /app


COPY --from=builder /app/bin/Release/jarvisAgent /app/jarvisAgent


COPY --chown=appuser:appuser config.json /app/config.json
COPY --chown=appuser:appuser web /app/web
COPY --chown=appuser:appuser scripts /app/scripts

RUN mkdir -p /app/queue && chown -R appuser:appuser /app

# Switch to non-root user
USER appuser

EXPOSE 8080

CMD ["./jarvisAgent"]

