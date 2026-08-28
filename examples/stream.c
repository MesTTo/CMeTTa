/* Purpose: show that an answer cursor is stepped rather than drained, which
 *   is what makes an endless MeTTa stream usable from C.
 * Guarantees: takes five answers from a generator that never ends, and
 *   returns; a binding that computed the whole answer set could not.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#define MT_SHORTHAND
#include <cetta.h>
#include <stdio.h>

int main(void)
{ metta *m = mt_open(NULL);
  mt_space *kb;
  int taken = 0;
  int i;

  if ( !m ) return fprintf(stderr, "boot: %s\n", mt_errmsg()), 1;

  /* Run for its effect: a definition's point is what it leaves behind, not
     what it answers. */
  mt_do(m, "(= (from $n) (superpose ($n (from (+ $n 1)))))");

  /* (from 0) is 0, 1, 2, ... forever. Five are wanted, so five are computed
     and the rest never run. Leaving the loop closes the cursor. */
  mt_each (a, mt_eval(m, E("from", 0)))
  { printf("take %d: %lld\n", ++taken, (long long)mt_int(a));
    if ( taken == 5 ) break;
  }

  /* A space is the same cursor shape over stored atoms, and the write verbs
     take their atom, so nothing here is dropped by hand. */
  if ( (kb = mt_space_open(m, "&stream-demo")) != NULL )
  { for (i = 0; i < 3; i++) mt_add(kb, E("edge", "a", i));

    printf("%zu stored\n", mt_count(kb));
    mt_each (a, mt_atoms(kb))
        printf("stored: %s\n", mt_show(a));

    /* A binding by the name you wrote, rather than by counting children. */
    mt_each_cursor (row, it, mt_match(kb, E("edge", "a", V("n"))))
    { (void)row;
      printf("n = %s\n", mt_show(mt_bound(it, "n")));
    }

    /* A pattern reused across calls is the one place mt_keep() earns its
       place: the door would otherwise take the only reference on the first
       pass and leave nothing for the second. */
    { mt_atom *pattern = E("edge", "a", V("n"));
      for (i = 0; i < 2; i++)
      { size_t seen = 0;
        mt_each (row, mt_match(kb, mt_keep(pattern))) { (void)row; seen++; }
        printf("pass %d matched %zu\n", i + 1, seen);
      }
      mt_drop(pattern);
    }
    mt_space_close(kb);
  }

  mt_close(m);
  return 0;
}
