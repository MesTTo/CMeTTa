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

# One spelling of the bound, implemented in bounded.sh, which every runner in
# this tree and a command typed by hand all reach. Spelled as the path here
# rather than through a `bounded` function, because `exec` cannot exec a
# function and this file's exit status must stay its delegate's.
exec sh "$HERE/../../bounded.sh" make --quiet -C "$HERE" all
