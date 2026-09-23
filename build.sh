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

# bounded.sh is the superproject's one definition of the ceiling and of the
# link to the process that started the command. A standalone checkout of this
# component has no ../../tools, and no enclosing gate either, so there the
# command runs as itself.
bounded() {
    if [ -f "$HERE/../../tools/bounded.sh" ]; then
        sh "$HERE/../../tools/bounded.sh" "$@"
    else
        "$@"
    fi
}
bounded make --quiet -C "$HERE" all
