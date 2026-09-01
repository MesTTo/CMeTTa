#!/bin/sh
# Purpose: rebuild and run the C seat under UBSan and standalone LSan without
#   mixing either instrumented object with the ordinary Makefile products.
# Assumes: clang, SWI-Prolog 10 development files, and root ai-tmp are writable.
# Guarantees: UBSan runs every current executable with recovery disabled;
#   standalone LSan preserves the main suite tally with exitcode=0 and refuses
#   a leak whose first frame after the allocator belongs to this seat
#   [tested: make -C extensions/cmetta sanitize; commit=b339084bb5625996fc88a31608d48ad31c575d1f].
# Owns resources: ai-tmp/cmetta-sanitize, replaced on each run; its temporary
#   quoted-path symlink is removed on every shell exit.

# shellcheck disable=SC2129

set -eu

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH='' cd -- "$HERE/../.." && pwd)
BUILD_ROOT="$ROOT/ai-tmp/cmetta-sanitize"
SWIPL=${SWIPL:-swipl}
CC=${SANITIZER_CC:-clang}

PLBASE=$($SWIPL --dump-runtime-variables 2>/dev/null \
    | sed -n 's/^PLBASE="\(.*\)";$/\1/p')
PLLIBDIR=$($SWIPL --dump-runtime-variables 2>/dev/null \
    | sed -n 's/^PLLIBDIR="\(.*\)";$/\1/p')

if [ ! -f "$PLBASE/include/SWI-Prolog.h" ]; then
    echo "sanitize: SWI-Prolog.h is absent under $PLBASE/include" >&2
    exit 1
fi

case "$BUILD_ROOT" in
    "$ROOT"/ai-tmp/cmetta-sanitize) ;;
    *) echo "sanitize: refusing unexpected build root $BUILD_ROOT" >&2; exit 1 ;;
esac
rm -rf -- "$BUILD_ROOT"
mkdir -p "$BUILD_ROOT"

# `$x` in mt_lower is an intentional compiler extension documented by the
# public macro. Clang alone diagnoses the token even while merely stringifying
# it, so suppress that one extension warning and keep every other warning fatal.
COMMON="-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -Wno-dollar-in-identifier-extension -fPIC -D_FORTIFY_SOURCE=2 -fstack-protector-strong -I$HERE -I$PLBASE/include"
ENGINE_DEFINE="-DMT_ENGINE_PATH=\"$ROOT\""
# Sanitizer callbacks are supplied by each executable's compiler runtime, so
# an instrumented shared object deliberately cannot use -z defs. The ordinary
# libraries do use it, and `make hardening` checks those linked artifacts.
LINK="-L$PLLIBDIR -Wl,-rpath,$PLLIBDIR -Wl,-z,relro,-z,now -lswipl -lm"
NORMAL_TESTS="test_cmetta test_bad_boot test_quoted_path"
FAULT_TESTS="test_alloc_failure test_cursor_ids test_reopen"
EXAMPLES="hello ops lower stream"
FIXTURE="$ROOT/ai-tmp/cmetta-sanitize-path-o'brien-unicodé-$$"

cleanup_fixture() {
    rm -f -- "$FIXTURE"
}
trap cleanup_fixture 0 1 2 15

build_matrix() {
    mode=$1
    sanitize_flags=$2
    out="$BUILD_ROOT/$mode"
    mkdir -p "$out/tests" "$out/examples"

    # The compiler flag strings are deliberately split into words.
    # shellcheck disable=SC2086
    $CC $COMMON "$ENGINE_DEFINE" $sanitize_flags -shared \
        -o "$out/libcmetta.so" "$HERE/cmetta.c" $sanitize_flags $LINK
    # shellcheck disable=SC2086
    $CC $COMMON "$ENGINE_DEFINE" $sanitize_flags -DMT_TEST_FAULTS -shared \
        -o "$out/tests/libcmetta_fault.so" "$HERE/cmetta.c" \
        $sanitize_flags $LINK

    for test_name in $NORMAL_TESTS; do
        # shellcheck disable=SC2086
        $CC $COMMON "$ENGINE_DEFINE" $sanitize_flags \
            -o "$out/tests/$test_name" "$HERE/tests/$test_name.c" \
            -L"$out" -Wl,-rpath,"$out" -lcmetta $sanitize_flags $LINK
    done
    for test_name in $FAULT_TESTS; do
        # shellcheck disable=SC2086
        $CC $COMMON "$ENGINE_DEFINE" $sanitize_flags -DMT_TEST_FAULTS \
            -o "$out/tests/$test_name" "$HERE/tests/$test_name.c" \
            -L"$out/tests" -Wl,-rpath,"$out/tests" -lcmetta_fault \
            $sanitize_flags $LINK
    done
    # shellcheck disable=SC2086
    $CC $COMMON "$ENGINE_DEFINE" $sanitize_flags \
        -o "$out/tests/test_threads" "$HERE/tests/test_threads.c" -pthread \
        -L"$out" -Wl,-rpath,"$out" -lcmetta $sanitize_flags $LINK
    for example_name in $EXAMPLES; do
        # shellcheck disable=SC2086
        $CC $COMMON "$ENGINE_DEFINE" $sanitize_flags \
            -o "$out/examples/$example_name" "$HERE/examples/$example_name.c" \
            -L"$out" -Wl,-rpath,"$out" -lcmetta $sanitize_flags $LINK
    done
}

run_ubsan() {
    out="$BUILD_ROOT/undefined"
    log="$out/output.log"
    build_matrix undefined "-fsanitize=undefined -fno-sanitize-recover=all"

    : > "$log"
    "$out/tests/test_cmetta" >> "$log" 2>&1
    "$out/tests/test_bad_boot" >> "$log" 2>&1
    rm -f -- "$FIXTURE"
    ln -s "$ROOT" "$FIXTURE"
    CMETTA_TEST_ENGINE_PATH="$FIXTURE" \
        "$out/tests/test_quoted_path" >> "$log" 2>&1
    cleanup_fixture
    "$out/tests/test_alloc_failure" >> "$log" 2>&1
    "$out/tests/test_cursor_ids" >> "$log" 2>&1
    "$out/tests/test_reopen" >> "$log" 2>&1
    "$out/tests/test_threads" >> "$log" 2>&1
    for example_name in $EXAMPLES; do
        "$out/examples/$example_name" > /dev/null 2>> "$log"
        echo "$example_name ok" >> "$log"
    done
    grep -Eq '[0-9]+ checks, 0 failures' "$log"
    echo "UndefinedBehaviorSanitizer"
    cat "$log"
    echo "UBSan diagnostics: none"
}

run_lsan() {
    out="$BUILD_ROOT/leak"
    log="$out/output.log"
    build_matrix leak "-fsanitize=leak"

    : > "$log"
    LSAN_OPTIONS=exitcode=0 "$out/tests/test_cmetta" >> "$log" 2>&1
    grep -Eq '[0-9]+ checks, 0 failures' "$log"
    if grep -Eq '^[[:space:]]*#1 .*[/ ](cmetta\.c|test_[^ /]*\.c):[0-9]+' "$log"; then
        cat "$log"
        echo "LeakSanitizer: a retained allocation originates in C-seat code" >&2
        exit 1
    fi
    echo "LeakSanitizer (standalone, LSAN_OPTIONS=exitcode=0)"
    grep -E 'checks, 0 failures|SUMMARY: LeakSanitizer' "$log"
    echo "C-seat allocation origins: none"
}

run_ubsan
run_lsan
