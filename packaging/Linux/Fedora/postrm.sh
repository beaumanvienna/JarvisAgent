#!/bin/bash
set -e

echo "==> Removing Python virtual environment ..."
rm -rf /opt/jarvisagent/.venv
echo "==> You may want to remove /opt/jarvisagent/ manually if custom data remains."
