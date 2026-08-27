#!/bin/sh
# Purpose: build this binding through its own Makefile, so the driver above
#   needs to know only that a component has a build.sh.
# Guarantees: the Makefile owns the prerequisite checks and refuses by name;
#   this only chooses the target and anchors the directory.
# Open Obligations:
#   To Do: None
#   Hacks: None
#   Future Enhancements: None

set -eu
HERE=$(cd -- "$(dirname -- "$0")" && pwd)
exec make --quiet -C "$HERE" all
