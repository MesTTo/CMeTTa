/* Purpose: exercise range, list-shape, exact-counter and stack-default
 *   contracts through the fault-only CMeTTa library.
 * Assumes: this binary links tests/libcmetta_fault.so, whose probes can build
 *   invalid engine terms that the public bridge never returns.
 * Guarantees: exits nonzero if allocation arithmetic wraps, an improper
 *   callback list is accepted, a negative count wraps, a counter loses bits,
 *   or clearing limits does not restore SWI's original stack limit.
 * Owns resources: drops its atom and closes the runtime before exit.
 */

#include <cmetta.h>

#include <stdint.h>
#include <stdio.h>

extern bool mt_test_improper_apply_is_rejected(void);
extern bool mt_test_negative_count_is_rejected(void);
extern bool mt_test_large_stats_are_exact(void);
extern bool mt_test_decode_growth_overflow_is_rejected(void);
extern size_t mt_test_stack_limit(void);

static int failures;

static void expect(bool condition, const char *claim)
{ if ( condition ) return;
  failures++;
  fprintf(stderr, "internal contract regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
}

int main(void)
{ metta *runtime = mt_open(NULL);
  mt_atom *dummy;
  mt_atom *children[1];
  size_t impossible = SIZE_MAX / sizeof(children[0]) + 1;
  size_t initial, bounded;

  expect(runtime != NULL, "the runtime must boot");
  if ( !runtime ) return 1;

  dummy = mt_sym("allocation-sentinel");
  children[0] = dummy;
  mt_clear();
  expect(mt_exprv(impossible, children) == NULL,
         "mt_exprv must reject wrapped allocation arithmetic before walking");
  expect(mt_error() == MT_NOMEM, "the impossible expression must name memory");
  mt_drop(dummy);

  mt_clear();
  expect(mt_test_decode_growth_overflow_is_rejected(),
         "decode vector growth must reject a wrapped capacity");

  mt_clear();
  expect(mt_test_improper_apply_is_rejected(),
         "an applied function must reject an improper argument list");

  mt_clear();
  expect(mt_test_negative_count_is_rejected(),
         "a negative engine count must not become a huge size_t");

  mt_clear();
  expect(mt_test_large_stats_are_exact(),
         "integer counters above 2^53 must retain every bit");

  mt_clear();
  initial = mt_test_stack_limit();
  bounded = initial > 16u * 1024u * 1024u
          ? initial / 2u : 32u * 1024u * 1024u;
  expect(initial > 0, "SWI's initial stack limit must be readable");
  expect(bounded != initial, "the stack-limit fixture must change the value");
  expect(mt_limit(runtime, (mt_limits){ .stack_bytes = bounded }),
         "a valid stack limit must be accepted");
  expect(mt_test_stack_limit() == bounded,
         "the accepted stack limit must reach SWI");
  expect(mt_limit(runtime, (mt_limits){0}),
         "a zero limit struct must clear every bound");
  expect(mt_test_stack_limit() == initial,
         "clearing limits must restore SWI's original stack ceiling");

  mt_close(runtime);
  if ( !failures ) puts("internal range, list, stats and limit contracts ok");
  return failures ? 1 : 0;
}
