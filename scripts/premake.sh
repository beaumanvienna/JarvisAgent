#!/usr/bin/env bash
# @jarvis-script
# @short: Run premake5 in the current working directory with a list of arguments
# @params: action [flags...]
# @description: Thin wrapper around premake5. Pass the action (gmake / gmake2 /
#   vs2022 / xcode4 / clean) followed by any number of flags in any order.
#   Flags the wrapper recognises (order-independent):
#     engine     → --engine          (default edition is studio — j9t-specific)
#     studio     → no flag (explicit; cancels a prior 'engine')
#     tracy      → --tracy
#   Unknown tokens are passed through verbatim so raw flags like --foo=bar keep
#   working if premake grows new options. Runs in the caller's current working
#   directory — `cd` into the project you want to configure before invoking,
#   or set the shell task's `working_directory` when calling from a JCWF.
# @outputs: Premake action output (Makefile, VS solution, xcodeproj, etc.)

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: scripts/premake.sh <action> [flags...]" >&2
  echo "  actions: gmake gmake2 vs2022 xcode4 clean" >&2
  echo "  flags:   engine studio tracy (or any raw --flag)" >&2
  exit 1
fi

action="$1"
shift

edition=""   # "" = studio (default); "--engine" = engine
tracy=""     # "" = off; "--tracy" = on
extra=()     # any raw flags the caller passes through

for token in "$@"; do
  case "$token" in
    engine)  edition="--engine" ;;
    studio)  edition="" ;;
    tracy)   tracy="--tracy" ;;
    *)       extra+=("$token") ;;
  esac
done

echo "[premake] cwd=$PWD action=$action edition=${edition:-studio} tracy=${tracy:-off} extra=${extra[*]:-}"
exec premake5 "$action" ${edition:+$edition} ${tracy:+$tracy} "${extra[@]}"
