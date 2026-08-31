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
# predicates, and consults the engine; extensions/cmetta/extension.pl sees
# '$cmetta_present'/0 and loads the bridge beside it. Because there is no
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
    binding="$HERE/extensions/cmetta"
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
    sh "$HERE/extensions/cmetta/test.sh"
}
run GATE c-binding check_c_binding

# The same seat's benchmarks. Skipped for the same three reasons as the suite
# above and for two more of its own, because a measurement needs instruments
# the suite does not: perf, to read instructions:u, and a Python that can
# import metta, because the counters are compared through metta's own
# BenchmarkBaseline rather than through a second harness copied here.
#
# THE COUNTER RULE, and it is why this lane exists separately from the pytest
# benchmark lanes: inference counters are BLIND across the C boundary, since
# foreign code retires no inferences at all. A C wire encoder in this tree once
# measured 526x faster on the inference counter while CPU time said it was 1.8x
# SLOWER. Every case here is therefore decided by `perf stat -e instructions:u`
# and CPU time PAIRED, and extensions/cmetta/benchmarks/bench.py says beside
# each case which counter decides it.
check_c_bench() {
    binding="$HERE/extensions/cmetta"
    [ -d "$binding" ] || return 0
    if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
        echo "note: no C compiler found, the C benchmark suite will not run" >&2
        return 0
    fi
    if [ ! -f "$(swipl --dump-runtime-variables 2>/dev/null \
                  | sed -n 's/^PLBASE="\(.*\)";$/\1/p')/include/SWI-Prolog.h" ]; then
        echo "note: SWI-Prolog development headers not found, the C benchmark \
suite will not run" >&2
        return 0
    fi
    if ! command -v perf >/dev/null 2>&1 || [ ! -x /usr/bin/setarch ]; then
        echo "note: perf or setarch not found, the C benchmark suite will not \
run; instructions:u is what decides these cases" >&2
        return 0
    fi
    if ! "$PY" -c 'import metta' >/dev/null 2>&1 &&
       ! ( cd "$HERE/extensions/python" && "$PY" -c 'import metta' ) >/dev/null 2>&1; then
        echo "note: this python cannot import metta, the C benchmark suite \
will not run; it compares through metta's BenchmarkBaseline" >&2
        return 0
    fi
    CHECK_PY="$PY" sh "$HERE/extensions/cmetta/bench.sh"
}

run GATE c-bench check_c_bench

# The install, proven by USING it. Everything above builds and runs in this
# checkout, where the engine tree is two directories up and the library is
# found by an rpath into the build directory; none of that is true for a
# consumer, and a C library nobody outside the tree can link against is a
# directory rather than a library. This lane installs into build/ under a real
# prefix, compiles tests/install_consumer.c against nothing but
# `pkg-config --cflags --libs cmetta`, and runs it with METTA_PATH unset. It
# needs pkg-config on top of the compiler and headers the lanes above need, and
# skips by name without it for the same reason they do.
check_c_install() {
    binding="$HERE/extensions/cmetta"
    [ -d "$binding" ] || return 0
    if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
        echo "note: no C compiler found, the C install check will not run" >&2
        return 0
    fi
    if ! command -v pkg-config >/dev/null 2>&1; then
        echo "note: pkg-config not found, the C install check will not run; it \
is how a consumer finds an installed library" >&2
        return 0
    fi
    if [ ! -f "$(swipl --dump-runtime-variables 2>/dev/null \
                  | sed -n 's/^PLBASE="\(.*\)";$/\1/p')/include/SWI-Prolog.h" ]; then
        echo "note: SWI-Prolog development headers not found, the C install \
check will not run" >&2
        return 0
    fi
    make --quiet -C "$HERE/extensions/cmetta" install-check
}
run GATE c-install check_c_install
