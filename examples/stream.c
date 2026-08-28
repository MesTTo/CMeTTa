/* Purpose: show that an answer cursor is stepped rather than drained, which
 *   is what makes an endless MeTTa stream usable from C.
 * Guarantees: takes five answers from a generator that never ends, and
 *   returns; a binding that computed the whole answer set could not.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#define CETTA_SHORTHAND
#include <cetta.h>
#include <stdio.h>

int main(void)
{ cetta *m = cetta_open(NULL);
  cetta_space *kb;
  int taken = 0;
  int i;

  if ( !m ) return fprintf(stderr, "boot: %s\n", cetta_errmsg()), 1;

  cetta_answers_free(cetta_run(m,
      "(= (from $n) (superpose ($n (from (+ $n 1)))))"));

  /* (from 0) is 0, 1, 2, ... forever. Five are wanted, so five are computed
     and the rest never run. Leaving the loop closes the cursor. */
  cetta_each (a, cetta_eval(m, E("from", 0)))
  { printf("take %d: %lld\n", ++taken, (long long)cetta_int(a));
    if ( taken == 5 ) break;
  }

  /* A space is the same cursor shape over stored atoms, and the write verbs
     take their atom, so nothing here is dropped by hand. */
  if ( (kb = cetta_space_open(m, "&stream-demo")) != NULL )
  { for (i = 0; i < 3; i++) cetta_add(kb, E("edge", "a", i));

    printf("%zu stored\n", cetta_count(kb));
    cetta_each (a, cetta_atoms(kb))
        printf("stored: %s\n", cetta_show(a));

    /* A pattern reused across calls is the one place cetta_keep() earns its
       place: the door would otherwise take the only reference on the first
       pass and leave nothing for the second. */
    { cetta_atom *pattern = E("edge", "a", V("n"));
      for (i = 0; i < 2; i++)
      { size_t seen = 0;
        cetta_each (row, cetta_match(kb, cetta_keep(pattern))) { (void)row; seen++; }
        printf("pass %d matched %zu\n", i + 1, seen);
      }
      cetta_drop(pattern);
    }
    cetta_space_close(kb);
  }

  cetta_close(m);
  return 0;
}
