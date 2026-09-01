/* Purpose: prove that a failed engine boot returns its Prolog exception to C,
 *   clears the pending ball, and leaves the process able to boot correctly.
 * Assumes: the source checkout named by MT_ENGINE_PATH contains engine/.
 * Guarantees: exits nonzero if a missing boot tree leaks an exception into
 *   the embedding host or prevents a later valid mt_open().
 * Owns resources: closes the successfully opened runtime before exit.
 */

#define MT_SHORTHAND
#include <cmetta.h>
#include <SWI-Prolog.h>

#include <stdio.h>
#include <string.h>

static int check(int condition, const char *claim)
{ if ( condition ) return 0;
  fprintf(stderr, "bad boot regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
  return 1;
}

int main(void)
{ const char *missing = "/definitely/not/a/cmetta-engine-o'brien-d2c9ddc3";
  mt_config bad = { .path = missing };
  metta *runtime;
  int failed = 0;

  failed |= check(mt_open(&bad) == NULL,
                  "a missing engine tree must refuse to boot");
  failed |= check(mt_error() == MT_ERROR,
                  "the boot refusal must be an engine error");
  failed |= check(mt_errmsg() && strstr(mt_errmsg(), missing),
                  "the error must name the missing tree");
  failed |= check(mt_errmsg() && strstr(mt_errmsg(), "exist"),
                  "the error must carry the engine's existence diagnosis");
  failed |= check(PL_exception(0) == 0,
                  "the failed call must clear SWI's pending exception");
  if ( failed ) return 1;

  mt_clear();
  runtime = mt_open(NULL);
  failed |= check(runtime != NULL,
                  "a valid boot must work after the caught exception");
  if ( runtime )
  { failed |= check(mt_one_int(mt_run(runtime, "!(+ 1 2)")) == 3,
                    "the recovered runtime must evaluate normally");
    failed |= check(mt_ok(), "the recovered evaluation must be error-free");
    mt_close(runtime);
  }

  if ( !failed ) puts("bad boot exception barrier ok");
  return failed ? 1 : 0;
}
