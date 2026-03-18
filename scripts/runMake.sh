#!/usr/bin/env bash
# @jarvis-script
# @short: Run make in the current working directory
# @description: Wrapper for 'make' — shell tasks must live under scripts/.
#   Input files (e.g. hello.c, Makefile) are materialized by the JCWF runtime.
set -euo pipefail

make
