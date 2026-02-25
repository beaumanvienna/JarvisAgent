#!/usr/bin/env bash
# runMake.sh — wrapper for 'make' (shell tasks must live under scripts/)
# Input files (hello.c, Makefile) are materialized by the JCWF runtime.
set -euo pipefail

make
