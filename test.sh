#!/bin/sh
# Purpose: run this binding's own tests, so a developer and the gate reach them
#   through one entry point. check.sh's c-binding lane calls this file; running
#   it by hand runs exactly what the gate runs.
# Assumes: swipl is on PATH with its development files, and a C compiler is
#   present. Neither is checked here: the Makefile owns the prerequisite checks
#   and refuses by NAME, which is what a developer running this directly wants
#   to be told. The GATE keeps its own skips, because a toolchain that is
#   absent is not a defect in the tree and a gate that failed for it would be
#   failing for a reason that is not the tree.
# Guarantees: builds from clean and exits nonzero on the first failing check or
#   example. Clean first because a stale binary from an older source passes a
#   suite that the current source would fail, and the C suite's whole job is to
#   answer for the current source.
# Open Obligations:
#   To Do: None
#   Hacks: None
#   Future Enhancements: None

set -eu
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
make --quiet -C "$HERE" clean >/dev/null 2>&1 || true
exec make --quiet -C "$HERE" test
