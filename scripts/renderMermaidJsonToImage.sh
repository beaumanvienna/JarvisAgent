#!/usr/bin/env bash
# @jarvis-script
# @short: Render a {title, mermaid} JSON object to an image via mmdc
# @params: INPUT_JSON — path to a JSON file with at least a 'mermaid' field
#   OUTPUT_IMAGE — path to the output image. Format inferred from extension:
#                  .svg (vector, recommended for docs), .png (raster), .pdf.
# @description: Extracts the 'mermaid' field from a JSON file and renders it
#   to the chosen format using mmdc (@mermaid-js/mermaid-cli). Companion to
#   mermaidMdToPdf.sh for the case when only a single image is needed
#   (no markdown wrapping). SVG output is preferred for documentation since
#   it scales without pixelation.
# @outputs: OUTPUT_IMAGE — the rendered diagram
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: renderMermaidJsonToImage.sh <input.json> <output.{svg|png|pdf}>" >&2
    exit 2
fi

INPUT_JSON="$1"
OUTPUT_IMAGE="$2"

if ! command -v mmdc >/dev/null 2>&1; then
    echo "ERROR: Missing dependency 'mmdc'" >&2
    echo "Install via: npm install -g @mermaid-js/mermaid-cli@10.x" >&2
    exit 1
fi

PYTHON_CMD="python3"
if ! command -v python3 >/dev/null 2>&1; then
    if command -v python >/dev/null 2>&1; then
        PYTHON_CMD="python"
    else
        echo "ERROR: python3 not found on PATH" >&2
        exit 1
    fi
fi

input_abs="$($PYTHON_CMD -c "import os,sys; print(os.path.abspath(sys.argv[1]))" "$INPUT_JSON")"
output_abs="$($PYTHON_CMD -c "import os,sys; print(os.path.abspath(sys.argv[1]))" "$OUTPUT_IMAGE")"
mkdir -p "$(dirname "$output_abs")"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

MMD_FILE="$WORK_DIR/diagram.mmd"

"$PYTHON_CMD" - "$input_abs" "$MMD_FILE" <<'PY'
import json, sys
src = sys.argv[1]
dst = sys.argv[2]
with open(src, encoding="utf-8") as f:
    data = json.load(f)
mermaid = data.get("mermaid")
if not isinstance(mermaid, str) or not mermaid.strip():
    print("ERROR: input JSON has no non-empty 'mermaid' field", file=sys.stderr)
    sys.exit(1)
with open(dst, "w", encoding="utf-8") as f:
    f.write(mermaid)
PY

# Sandbox-friendly mmdc args (Docker / CI). Same convention as mermaidMdToPdf.sh.
PUPPETEER_ARGS=()
if [ -f "/etc/mmdc-puppeteer-config.json" ]; then
    PUPPETEER_ARGS=("-p" "/etc/mmdc-puppeteer-config.json")
fi

echo "[renderMermaidJsonToImage.sh] Rendering $(basename "$input_abs") -> $(basename "$output_abs")"
mmdc -i "$MMD_FILE" -o "$output_abs" --backgroundColor white "${PUPPETEER_ARGS[@]}"
echo "[renderMermaidJsonToImage.sh] Done: $output_abs"
