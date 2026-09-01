/* Purpose: close and restart the embedded engine in one process while
 *   observing that runtime-owned SWI predicate handles are discarded.
 * Assumes: this binary links tests/libcmetta_fault.so, whose test-only getter
 *   exposes the cached handle value without dereferencing it.
 * Guarantees: exits nonzero if shutdown leaves the old cache published, the
 *   restarted runtime does not resolve its own handle, or decoding then fails.
 * Owns resources: drops decoded atoms and closes each successfully opened
 *   runtime before exit.
 */

#define MT_SHORTHAND
#include <cmetta.h>
#include <SWI-Prolog.h>

#include <stdio.h>
#include <string.h>

extern void *mt_test_cached_space_predicate(void);

static int failures;

static void expect(int condition, const char *claim)
{ if ( condition ) return;
  failures++;
  fprintf(stderr, "restart regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
}

static int cancel_next_cleanup_once(void)
{ term_t goal = PL_new_term_ref();
  const char *source =
    "at_halt((flag(cmetta_test_cleanup_cancel,N,N+1),"
    "(N=:=0->cancel_halt(cmetta_restart_test);true)))";
  return goal && PL_chars_to_term(source, goal) && PL_call(goal, NULL);
}

static void test_restart_replaces_runtime_owned_predicates(void)
{ metta *runtime = mt_open(NULL);
  void *first;
  mt_atom *space;

  expect(runtime != NULL, "the first runtime must boot");
  if ( !runtime ) return;
  first = mt_test_cached_space_predicate();
  expect(first != NULL, "the first runtime must own a predicate handle");
  expect(first == (void *)PL_predicate("metta_c_space_operand", 1, "user"),
         "the cache must name the current runtime's predicate");

  space = mt_one(mt_run(runtime, "!(new-space)"));
  expect(space && mt_kind_of(space) == MT_SPACE,
         "the first runtime must classify a space through its cache");
  mt_drop(space);

  expect(cancel_next_cleanup_once(),
         "the test must install its one-shot cleanup cancellation");
  mt_clear();
  mt_close(runtime);
  expect(PL_is_initialised(NULL, NULL),
         "a canceled cleanup must leave SWI initialised");
  expect(mt_test_cached_space_predicate() == first,
         "a canceled cleanup must retain the live predicate handle");
  expect(mt_error() == MT_ERROR && mt_errmsg() &&
         strstr(mt_errmsg(), "canceled"),
         "a canceled cleanup must report that the runtime remains open");
  mt_clear();
  expect(mt_one_int(mt_run(runtime, "!(+ 1 2)")) == 3,
         "the runtime must remain usable after canceled cleanup");

  mt_clear();
  mt_close(runtime);
  expect(!PL_is_initialised(NULL, NULL),
         "successful mt_close must finish SWI cleanup");
  expect(mt_test_cached_space_predicate() == NULL,
         "the invalidated predicate handle must not remain published");
  expect(mt_ok(), "successful cleanup must not invent an error");

  runtime = mt_open(NULL);
  expect(runtime != NULL, "the runtime must restart in the same process");
  if ( !runtime ) return;
  expect(mt_test_cached_space_predicate() != NULL,
         "the restarted runtime must resolve a fresh predicate handle");
  expect(mt_test_cached_space_predicate() ==
         (void *)PL_predicate("metta_c_space_operand", 1, "user"),
         "the restarted cache must name the current procedure table");

  space = mt_one(mt_run(runtime, "!(new-space)"));
  expect(space && mt_kind_of(space) == MT_SPACE,
         "space decoding must use the restarted runtime's handle");
  mt_drop(space);
  mt_close(runtime);
}

int main(void)
{ test_restart_replaces_runtime_owned_predicates();
  if ( !failures ) puts("runtime predicate restart ok");
  return failures ? 1 : 0;
}
