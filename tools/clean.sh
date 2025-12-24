#!/usr/bin/env bash
set -euo pipefail

rm -rfv ../workflows/myapp 
rm -rfv ../workflows/aiCarMaintenancePipeline
rm -rfv ../workflows/jarvisCppDocu
rm -rfv ../workflows/*.a 
rm -rfv ../workflows/*.o 
rm -rfv ../queue/*

echo "=== Done ==="
ls -Rlah ../workflows ../queue/ ./scripts/ ./bin/Release
