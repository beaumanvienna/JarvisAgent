#!/bin/bash
set -e

echo "==> Creating Python virtual environment in /opt/jarvisagent/.venv ..."
python3 -m venv /opt/jarvisagent/.venv
/opt/jarvisagent/.venv/bin/pip install --quiet --upgrade pip

echo "==> Installing Python tools (markitdown, md2pdf-mermaid, playwright) ..."
/opt/jarvisagent/.venv/bin/pip install --quiet "markitdown[all]" md2pdf-mermaid playwright

echo "==> Installing Playwright Chromium ..."
/opt/jarvisagent/.venv/bin/playwright install chromium

echo ""
echo "==> JarvisAgent installed to /opt/jarvisagent/"
echo ""
echo "    To get started:"
echo "      1. sudo cp /opt/jarvisagent/config.json.example /opt/jarvisagent/config.json"
echo "      2. Edit config.json (set API keys, queue/workflow paths)"
echo "      3. Run: jarvisagent"
echo ""
