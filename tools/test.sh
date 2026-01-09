#!/usr/bin/env bash
set -euo pipefail

echo "=== Before ==="
date
ls -Rlah ../workflows ../queue/ ./scripts/ ./bin/Release
echo "=== 1st run ==="
./bin/Release/jarvisAgent
echo "=== After 1st run ==="
ls -Rlah ../workflows ../queue/ ./scripts/ ./bin/Release
cat /tmp/log.txt 
echo "=== Done ==="
