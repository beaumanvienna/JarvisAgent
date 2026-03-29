#!/usr/bin/env bash
# @jarvis-script
# @short: Convert Markdown (with Mermaid diagrams) to PDF using mmdc + pandoc
# @params: INPUT_MD — path to the input Markdown file (may contain ```mermaid``` blocks)
#   OUTPUT_PDF — path to the output PDF file
# @description: Pre-renders Mermaid code blocks to PNG images using mmdc
#   (@mermaid-js/mermaid-cli), then converts the resulting Markdown to PDF
#   using pandoc with the pdflatex engine.
#   No Playwright dependency — mmdc bundles its own Chromium via Puppeteer,
#   updated independently of the Python ecosystem (~monthly releases).
# @outputs: OUTPUT_PDF — the generated PDF file
set -euo pipefail

# ── Dependency checks ─────────────────────────────────────────────────────────

_check_dep() {
    local cmd="$1"
    local hint_linux="$2"
    local hint_mac="$3"
    local hint_win="$4"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: Missing dependency '$cmd'" >&2
        case "$(uname -s 2>/dev/null)" in
            Darwin*)               echo "Install via: $hint_mac"   >&2 ;;
            CYGWIN*|MINGW*|MSYS*)  echo "Install via: $hint_win"   >&2 ;;
            *)                     echo "Install via: $hint_linux" >&2 ;;
        esac
        exit 1
    fi
}

_check_dep "mmdc" \
    "npm install -g @mermaid-js/mermaid-cli@10.x" \
    "npm install -g @mermaid-js/mermaid-cli@10.x" \
    "npm install -g @mermaid-js/mermaid-cli@10.x"

_check_dep "pandoc" \
    "apt install pandoc texlive-latex-base texlive-latex-extra texlive-fonts-recommended" \
    "brew install pandoc basictex" \
    "choco install pandoc miktex"

# ── Argument parsing ──────────────────────────────────────────────────────────

if [ "$#" -lt 2 ]; then
    echo "Usage: mermaidMdToPdf.sh <input.md> <output.pdf>" >&2
    exit 2
fi

INPUT_MD_PATH="$1"
shift
OUTPUT_PDF_PATH="$*"

# ── Python detection ──────────────────────────────────────────────────────────

PYTHON_CMD="python3"
if ! command -v python3 >/dev/null 2>&1; then
    if command -v python >/dev/null 2>&1; then
        PYTHON_CMD="python"
    else
        echo "ERROR: python3 not found on PATH" >&2
        exit 1
    fi
fi

# ── Resolve absolute paths ────────────────────────────────────────────────────

input_abs="$($PYTHON_CMD -c "import os,sys; print(os.path.abspath(sys.argv[1]))" "$INPUT_MD_PATH")"
output_abs="$($PYTHON_CMD -c "import os,sys; print(os.path.abspath(sys.argv[1]))" "$OUTPUT_PDF_PATH")"
mkdir -p "$(dirname "$output_abs")"

# ── Create temp working directory ─────────────────────────────────────────────

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# ── Step 1: Extract Mermaid blocks → .mmd files + patched .md ─────────────────

"$PYTHON_CMD" - "$input_abs" "$WORK_DIR" <<'PY'
import re, sys, os

md_path  = sys.argv[1]
work_dir = sys.argv[2]

with open(md_path, encoding="utf-8") as f:
    content = f.read()

pattern = re.compile(r'```mermaid\s*\n(.*?)```', re.DOTALL)
counter = [0]

def extract(m):
    idx = counter[0]
    counter[0] += 1
    mmd = os.path.join(work_dir, f"d{idx:02d}.mmd")
    png = os.path.join(work_dir, f"d{idx:02d}.png")
    with open(mmd, "w", encoding="utf-8") as f:
        f.write(m.group(1).strip())
    return f"![Diagram {idx}]({png})"

patched = pattern.sub(extract, content)

with open(os.path.join(work_dir, "input.md"), "w", encoding="utf-8") as f:
    f.write(patched)
PY

# ── Step 2: Render each .mmd → PNG via mmdc ───────────────────────────────────

# In sandboxed environments (Docker / CI) mmdc's Puppeteer needs --no-sandbox.
# Drop a config file at /etc/mmdc-puppeteer-config.json during image build and
# the script picks it up automatically; on developer machines the file won't
# exist and mmdc runs with its defaults.
PUPPETEER_ARGS=()
if [ -f "/etc/mmdc-puppeteer-config.json" ]; then
    PUPPETEER_ARGS=("-p" "/etc/mmdc-puppeteer-config.json")
fi

for mmd_file in "$WORK_DIR"/d*.mmd; do
    [ -f "$mmd_file" ] || continue
    png_file="${mmd_file%.mmd}.png"
    echo "[mermaidMdToPdf.sh] Rendering $(basename "$mmd_file") ..."
    mmdc -i "$mmd_file" -o "$png_file" --backgroundColor white "${PUPPETEER_ARGS[@]}"
done

# ── Step 3: Convert patched MD → PDF via pandoc (pdflatex) ───────────────────

echo "[mermaidMdToPdf.sh] Converting to PDF via pandoc ..."
pandoc "$WORK_DIR/input.md" \
    -o "$output_abs" \
    --pdf-engine=pdflatex \
    -V geometry:margin=2cm \
    -V fontsize=11pt \
    --standalone

echo "[mermaidMdToPdf.sh] Done: $output_abs"
