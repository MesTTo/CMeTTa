/* Purpose: inject one eager-answer vector growth failure and verify that the
 *   C binding keeps the old allocation reachable for complete cleanup.
 * Assumes: this binary links tests/libcmetta_fault.so, built from cmetta.c
 *   with MT_TEST_FAULTS; the installed library has no injection symbol.
 * Guarantees: exits nonzero if ninth-answer growth crashes, leaks ownership
 *   into an unusable cursor, reports the wrong status, or poisons the engine.
 * Owns resources: closes every returned cursor and the runtime before exit.
 */

#define MT_SHORTHAND
#include <cmetta.h>

#include <stdio.h>

extern void mt_test_fail_eager_grow_after(size_t successful_grows);

static int failures;

static void expect(int condition, const char *claim)
{ if ( condition ) return;
  failures++;
  fprintf(stderr, "allocation regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
}

static void test_eager_growth_is_transactional(metta *runtime)
{ mt_answers *answers;

  mt_clear();
  mt_test_fail_eager_grow_after(0);
  answers = mt_run(runtime, "!(superpose (0 1 2 3 4 5 6 7 8))");
  expect(answers == NULL, "the injected vector growth must fail the run");
  expect(mt_error() == MT_NOMEM, "allocation failure must report MT_NOMEM");
  expect(mt_errmsg() && mt_errmsg()[0], "allocation failure must say why");
  mt_answers_free(answers);

  mt_clear();
  expect(mt_one_int(mt_run(runtime, "!(+ 20 22)")) == 42,
         "the engine must remain usable after failed answer collection");
  expect(mt_ok(), "the recovery evaluation must leave a clean error state");
}

int main(void)
{ metta *runtime = mt_open(NULL);
  if ( !runtime )
  { fprintf(stderr, "allocation regression could not boot: %s\n",
            mt_errmsg() ? mt_errmsg() : "(none)");
    return 1;
  }

  test_eager_growth_is_transactional(runtime);
  mt_close(runtime);
  if ( !failures ) puts("transactional eager growth ok");
  return failures ? 1 : 0;
}
