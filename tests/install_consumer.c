/* Purpose: be the program `make install-check` compiles against an INSTALLED
 *   libcmetta, knowing nothing about this checkout but what pkg-config says.
 *
 * Assumes: cmetta.h is on the include path and libcmetta on the link path,
 *   both supplied by `pkg-config --cflags --libs cmetta`. It does NOT assume
 *   $METTA_PATH: the whole claim being checked is that an installed library
 *   finds the installed engine on its own, because `make install` bakes the
 *   installed engine's directory into it.
 * Guarantees: exits 0 having printed the one answer to `(+ 2 3)`, and exits
 *   nonzero naming what failed otherwise
 *   [tested: extensions/cmetta/check.sh c-install; commit=1c40a5f96c308941b4c0669594acb06403109751].
 * Fails when: the engine tree was not installed beside the library, which is
 *   the failure this exists to catch and the reason it prints mt_errmsg().
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#include <cmetta.h>

#include <stdio.h>

_Static_assert(__STDC_VERSION__ >= 201112L,
               "cmetta's pkg-config metadata must select C11 or newer");

int main(void)
{ metta *m = mt_open(NULL);
  int64_t answer;

  if ( !m )
  { printf("boot failed: %s\n", mt_errmsg() ? mt_errmsg() : "(no message)");
    return 1;
  }
  answer = mt_one_int(mt_eval(m, mt_expr("+", 2, 3)));
  if ( !mt_ok() )
  { printf("evaluation failed: %s\n", mt_errmsg() ? mt_errmsg() : "(no message)");
    mt_close(m);
    return 1;
  }
  printf("%lld\n", (long long)answer);
  mt_close(m);
  return 0;
}
