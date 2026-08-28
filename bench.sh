#!/bin/sh
# Purpose: measure what a C host pays for this binding and hold every case to
#   benchmarks/baseline.json. check.sh's c-bench lane calls this file; running
#   it by hand runs exactly what the gate runs.
# Assumes:
#   - a C compiler and SWI's development files, which the Makefile checks and
#     refuses by name
#   - perf and setarch, which metta.benchmarking checks and refuses by name;
#     perf_event_paranoid must allow instructions:u, which needs no privilege
#     at -1 and none at 2 for a process the caller owns
#   - a Python that can import metta from extensions/python. $CHECK_PY picks
#     it, the same variable check.sh reads, and the same default list.
# Guarantees:
#   - the driver is built before it is measured, so a fresh checkout needs one
#     command rather than two
#   - exits nonzero when any case leaves its band, in either direction
#   - passes its arguments through, so `sh bench.sh boot term-in` measures two
#     cases and `sh bench.sh --update` re-pins every one
# Decides: the counters are `perf stat -e instructions:u` and CPU time, PAIRED.
#   Inference counters are BLIND across the C boundary because foreign code
#   retires none, and this tree has a C encoder that measured 526x faster on
#   the inference counter while CPU time said it was 1.8x slower. The reason
#   each case is decided the way it is sits in benchmarks/bench.py beside that
#   case.
# Open Obligations:
#   To Do: None
#   Hacks: None
#   Future Enhancements: None

set -eu
HERE=$(cd -- "$(dirname -- "$0")" && pwd)

PY=${CHECK_PY:-}
if [ -z "$PY" ]; then
    for candidate in "$HOME/Dev/.venv-pypetta/bin/python" \
                     "$HERE/../../.venv/bin/python" python3; do
        if command -v "$candidate" >/dev/null 2>&1; then PY="$candidate"; break; fi
    done
fi
command -v "$PY" >/dev/null 2>&1 || {
    echo "bench.sh: no python found (set CHECK_PY)" >&2
    exit 2
}

make --quiet -C "$HERE" bench
exec "$PY" "$HERE/benchmarks/bench.py" "$@"
