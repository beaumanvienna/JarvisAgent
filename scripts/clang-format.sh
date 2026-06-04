#!/usr/bin/env bash
set -euo pipefail

# Determine repo root as the parent of the script directory
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repoRoot="$(cd "${scriptDir}/.." && pwd)"

if [[ ! -f "${repoRoot}/.clang-format" ]]; then
    echo "ERROR: .clang-format not found in repo root: ${repoRoot}"
    exit 1
fi

if ! command -v clang-format >/dev/null 2>&1; then
    echo "ERROR: clang-format not found on PATH."
    exit 1
fi

echo "clang-format: $(clang-format --version)"
echo "Repo root: ${repoRoot}"

# Collect C/C++ source files safely (handles spaces)
mapfile -d '' sourceFiles < <(
    find "${repoRoot}/code/backend/application" "${repoRoot}/code/backend/engine" \
        -type f \
        \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" -o -name "*.cxx" \
           -o -name "*.h" -o -name "*.hh" -o -name "*.hpp" -o -name "*.hxx" \) \
        -print0
)

if [[ ${#sourceFiles[@]} -eq 0 ]]; then
    echo "No source files found under code/backend/application/ and code/backend/engine/."
    exit 0
fi

echo "Found ${#sourceFiles[@]} files to format."

clang-format -i --style=file "${sourceFiles[@]}"

echo "Formatting complete."
echo "Review with: git diff"
