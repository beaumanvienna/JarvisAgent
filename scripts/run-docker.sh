#!/usr/bin/env bash
# @jarvis-script
# @short: Pull and run JarvisAgent via Docker (interactive or headless, HTTP or TLS)
# @params: [--headless] [--tls] [data_dir]
# @description: Wrapper around `docker run ghcr.io/.../jarvisagent`. Handles
#   docker group membership (re-execs with the group activated when needed),
#   mounts a per-user data directory so workflows persist across restarts, and
#   supports both interactive-TUI and headless (web-only) modes. Default is
#   plain HTTP on port 8080; pass --tls to serve HTTPS on port 8443 (requires
#   certs under <data_dir>/certs/ — see doc/jarvisagent.md DOCKER section).
#   data_dir defaults to ~/JarvisAgent.
# @outputs: A running JarvisAgent container with web UI on localhost:8080 (or 8443 with --tls)
#
# Usage:
#   ./scripts/run-docker.sh                    # interactive with TUI, HTTP on 8080
#   ./scripts/run-docker.sh --headless         # headless (no TUI), HTTP on 8080
#   ./scripts/run-docker.sh --tls              # interactive + TUI, HTTPS on 8443
#   ./scripts/run-docker.sh --headless --tls   # headless, HTTPS on 8443
#   ./scripts/run-docker.sh /path              # custom data directory
#   ./scripts/run-docker.sh --tls /path        # HTTPS + custom data directory

set -euo pipefail

# ---- Pre-flight: check Docker is available ----
if ! command -v docker &>/dev/null; then
    echo "ERROR: 'docker' not found. Please install Docker first:"
    echo "  https://docs.docker.com/engine/install/"
    exit 1
fi

if ! docker info &>/dev/null; then
    echo "Cannot connect to the Docker daemon."
    echo ""
    if [[ "$(uname -s)" == "Linux" ]]; then
        if ! id -nG "$USER" | grep -qw docker; then
            echo "Your user is not in the 'docker' group. Adding now (requires sudo)..."
            sudo usermod -aG docker "$USER"
        fi
        # Group may exist but not be active in this shell — re-exec under it.
        # The guard variable prevents an infinite loop if the daemon is genuinely stopped.
        if id -nG "$USER" | grep -qw docker && [ -z "${_RUN_DOCKER_REEXEC:-}" ]; then
            echo "==> Activating docker group..."
            export _RUN_DOCKER_REEXEC=1
            exec sg docker -c "$0 $*"
        fi
        echo "ERROR: Docker daemon is not running. Please start Docker first."
    else
        echo "ERROR: Docker daemon is not running."
        echo "       On macOS, start Docker Desktop from Applications."
    fi
    exit 1
fi

IMAGE="ghcr.io/beaumanvienna/jarvisagent:latest"
HEADLESS=false
TLS=false
SAW_DASH_DASH=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --headless) HEADLESS=true; shift ;;
        --tls)      TLS=true;      shift ;;
        --) shift; SAW_DASH_DASH=true; break ;;
        -*) echo "ERROR: Unknown option: $1" >&2; exit 1 ;;
        *)  break ;;
    esac
done

# After option parsing, decide the data_dir and leave "$@" with passthrough
# container args only. Three cases:
#   1. $SAW_DASH_DASH — remaining args are all passthrough; data_dir defaults.
#   2. $1 present — that's the data_dir positional; consume it.
#   3. No args left — data_dir defaults; nothing to pass through.
if $SAW_DASH_DASH; then
    DATA_DIR="$HOME/JarvisAgent"
else
    DATA_DIR="${1:-$HOME/JarvisAgent}"
    [[ $# -gt 0 ]] && shift
fi

if $TLS; then
    PORT=8443
    SCHEME=https
else
    PORT=8080
    SCHEME=http
fi

echo "==> Data directory: $DATA_DIR"
mkdir -p "$DATA_DIR"

# ---- TLS pre-flight: verify cert files are present on the host ----
if $TLS; then
    CERT="$DATA_DIR/certs/j9t-cert.pem"
    KEY="$DATA_DIR/certs/j9t-key.pem"
    if [ ! -f "$CERT" ] || [ ! -f "$KEY" ]; then
        cat <<EOF >&2

ERROR: --tls requires TLS certificate files in the data directory.
Expected paths:
    $CERT
    $KEY

Generate a self-signed pair:
    mkdir -p "$DATA_DIR/certs"
    openssl req -x509 -newkey rsa:2048 -nodes \\
      -keyout "$KEY" \\
      -out    "$CERT" \\
      -days 365 \\
      -subj "/CN=localhost" \\
      -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"
    chmod 600 "$KEY"

For more detail see the DOCKER section of the user manual
(doc/jarvisagent.md, or \`man jarvisagent\` on Linux/macOS) and
certs/README.md.
EOF
        exit 1
    fi
fi

echo "==> Pulling $IMAGE"
if ! docker pull "$IMAGE" 2>/dev/null; then
    if docker image inspect "$IMAGE" &>/dev/null; then
        echo "WARNING: Pull failed (no internet?). Using cached image."
    else
        echo "ERROR: Pull failed and no cached image found. Check your internet connection."
        exit 1
    fi
fi

echo "==> Starting JarvisAgent"
echo "    Dashboard: $SCHEME://localhost:$PORT"
echo "    Editor:    $SCHEME://localhost:$PORT/editor"
echo ""

# Check that the target host port is free (Linux: ss; macOS: lsof)
PORT_IN_USE=false
if command -v ss &>/dev/null; then
    ss -tlnp 2>/dev/null | grep -q ":$PORT " && PORT_IN_USE=true
elif command -v lsof &>/dev/null; then
    lsof -iTCP:"$PORT" -sTCP:LISTEN &>/dev/null && PORT_IN_USE=true
fi
if $PORT_IN_USE; then
    echo "ERROR: Port $PORT is already in use. Stop the other process first."
    exit 1
fi

DOCKER_ARGS=(--rm -p "$PORT:$PORT" -v "$DATA_DIR:/app")
if $TLS; then
    DOCKER_ARGS+=(-e J9T_TLS=1)
fi
if $HEADLESS; then
    echo "    Mode: headless (no TUI)"
    exec docker run "${DOCKER_ARGS[@]}" "$IMAGE" "$@"
else
    exec docker run -it "${DOCKER_ARGS[@]}" "$IMAGE" "$@"
fi
