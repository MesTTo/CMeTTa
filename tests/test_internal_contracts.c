/* Purpose: exercise range, list-shape, exact-counter and stack-default
 *   contracts through the fault-only CMeTTa library.
 * Assumes: this binary links tests/libcmetta_fault.so, whose probes can build
 *   invalid engine terms that the public bridge never returns.
 * Guarantees: exits nonzero if allocation arithmetic wraps, an improper
 *   callback list is accepted, a foreign native handle bypasses its codec
 *   guard, a compound decodes outside the shared wire grammar, a cyclic answer
 *   is walked instead of refused, a long list costs references per cell, a
 *   negative count wraps, a counter loses bits, or clearing limits does not
 *   restore SWI's original stack limit.
 * Owns resources: drops its atom and closes the runtime before exit.
 */

#include <cmetta.h>

#include <stdint.h>
#include <stdio.h>

extern bool mt_test_improper_apply_is_rejected(void);
extern bool mt_test_native_handle_codec_round_trips(void);
extern bool mt_test_close_handshake_skips_erase(void);
extern bool mt_test_long_list_compound_decodes(size_t length);
extern bool mt_test_wire_grammar(void);
extern bool mt_test_cyclic_answer_refused(void);
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

static void test_native_handle_decode_and_encode_contract(void)
{ mt_clear();
  expect(mt_test_native_handle_codec_round_trips(),
         "a foreign native blob must decode as MT_HANDLE and encode back as "
         "the same blob, while a handle holding no term refuses encoding");
  expect(mt_error() == MT_UNSUPPORTED,
         "the printed-only handle's refusal must report MT_UNSUPPORTED");
}

static void test_a_handle_released_during_close_leaves_its_record(void)
{ mt_clear();
  expect(mt_test_close_handshake_skips_erase(),
         "a handle released while a close is announced must leave its record "
         "to cleanup, and one released otherwise must erase it");
}

/* A compound's key walk once made three term references per list cell and
   kept them until it returned, so a compound over a long list needed stack
   room for the list twice: 400,000 cells failed with out of memory at every
   limit from 16 to 32 MB. Decoded as an expression, the list is one level of
   the walk, one reference. */
static void test_a_long_list_compound_decodes_in_constant_references(metta *runtime)
{ mt_clear();
  expect(mt_limit(runtime, (mt_limits){ .stack_bytes = 24u * 1024u * 1024u }),
         "a 24 MB stack limit must be accepted");
  expect(mt_test_long_list_compound_decodes(400000),
         "a compound over a 400,000-element list must decode as an expression "
         "under a 24 MB stack limit, which holds the list once");
  expect(mt_limit(runtime, (mt_limits){0}), "clearing the limit must succeed");
}

static void test_compounds_decode_in_the_shared_wire_grammar(void)
{ mt_clear();
  expect(mt_test_wire_grammar(),
         "every compound shape must decode to the expression the Python and "
         "Node seats read it as: (F args...), (cons Head Tail), (F) for zero "
         "arity, a shared variable one variable, a functor a symbol");
}

static void test_a_cyclic_answer_is_refused_by_name(void)
{ mt_clear();
  expect(mt_test_cyclic_answer_refused(),
         "a rational-tree answer must be refused by name, not walked forever, "
         "while a finite one still decodes");
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

  test_native_handle_decode_and_encode_contract();
  test_a_handle_released_during_close_leaves_its_record();
  test_a_long_list_compound_decodes_in_constant_references(runtime);
  test_compounds_decode_in_the_shared_wire_grammar();
  test_a_cyclic_answer_is_refused_by_name();

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
