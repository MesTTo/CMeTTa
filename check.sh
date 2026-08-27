# Purpose: this component's own gate lanes, in the root gate's vocabulary.
# Assumes: it is SOURCED by check.sh, not executed. That is what lets it use
#   `run`, `$HERE` and the shared summary table, so one component's lanes cannot
#   report their own status differently from another's, and a child's exit code
#   cannot be lost on the way back up -- the hazard a driver that EXECUTES its
#   children has to solve and this one does not have.
# Guarantees: every path a lane runs is written literally. tests/checks/
#   evidence_runners.py models which files a lane covers by READING this text
#   and resolving $HERE/, so a path reached through a local variable is a path
#   the evidence gate cannot see.
# Open Obligations:
#   To Do: None
#   Hacks: None
#   Future Enhancements: None

# The C binding, the seam's third consumer and the only one that is IN the
# engine's process. A C main() calls PL_initialise, registers its foreign
# predicates, and consults the engine; bindings/cetta/decider.pl sees
# '$cetta_present'/0 and loads the bridge beside it. Because there is no
# language boundary to cross, this seat reads engine terms directly and has no
# wire codec, so the codec kit cannot gate it; what gates it instead is its own
# C suite here and the cross-seat parity case in the pytest lane above, which
# requires this binding and the Python host to answer the same programs.
#
# It needs a C compiler and SWI's development headers. Neither is fetched: a
# gate that reaches the network is a gate that fails for a reason that is not
# the tree, so a missing step is named and skipped, the same shape the C
# extension example and the Node lane take.
check_c_binding() {
    binding="$HERE/bindings/cetta"
    [ -d "$binding" ] || return 0
    if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
        echo "note: no C compiler found, the C binding suite will not run" >&2
        return 0
    fi
    if [ ! -f "$(swipl --dump-runtime-variables 2>/dev/null \
                  | sed -n 's/^PLBASE="\(.*\)";$/\1/p')/include/SWI-Prolog.h" ]; then
        echo "note: SWI-Prolog development headers not found, the C binding \
suite will not run" >&2
        return 0
    fi
    ( cd "$binding" && make --quiet clean >/dev/null 2>&1
      cd "$binding" && make --quiet test )
}
run GATE c-binding check_c_binding
