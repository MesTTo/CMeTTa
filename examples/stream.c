/* Purpose: show that an answer cursor is stepped rather than drained, which
 *   is what makes an endless MeTTa stream usable from C.
 * Guarantees: takes five answers from a generator that never ends, and
 *   returns; a binding that computed the whole answer set could not.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#include <cetta.h>
#include <stdio.h>

int main(void)
{ cetta_t *m;
  cetta_answers_t *answers = NULL;
  cetta_atom_t *goal;
  cetta_space_t *kb;
  int taken = 0;

  if ( cetta_open(NULL, &m) != CETTA_OK )
  { fprintf(stderr, "boot: %s\n", cetta_errmsg());
    return 1;
  }

  if ( cetta_run(m, "(= (from $n) (superpose ($n (from (+ $n 1)))))\n",
                 &answers) != CETTA_OK )
  { fprintf(stderr, "define: %s\n", cetta_errmsg());
    return 1;
  }
  cetta_answers_free(answers);

  /* (from 0) is 0, 1, 2, ... forever. Five are wanted, so five are computed
     and the rest never run. */
  goal = cetta_expr(2, cetta_sym("from"), cetta_int(0));
  if ( cetta_eval(cetta_self(m), goal, &answers) != CETTA_OK )
  { fprintf(stderr, "eval: %s\n", cetta_errmsg());
    return 1;
  }
  while ( taken < 5 && cetta_answers_step(answers) == CETTA_ROW )
  { int64_t v = 0;
    cetta_int_value(cetta_answers_atom(answers), &v);
    printf("take %d: %lld\n", ++taken, (long long)v);
  }
  /* Abandoning the cursor releases the engine behind it. */
  cetta_answers_free(answers);
  cetta_release(goal);

  /* The same cursor shape over stored atoms. */
  if ( cetta_space_open(m, "&stream-demo", &kb) == CETTA_OK )
  { int i;
    for (i = 0; i < 3; i++)
    { /* cetta_add takes a const pointer, so it BORROWS: the caller still owns
         the atom and releases it. Passing a constructor call inline here
         would leak, which is the ownership law doing its job. */
      cetta_atom_t *fact = cetta_expr(3, cetta_sym("edge"), cetta_sym("a"),
                                      cetta_int(i));
      cetta_add(kb, fact);
      cetta_release(fact);
    }
    if ( cetta_space_atoms(kb, &answers) == CETTA_OK )
    { while ( cetta_answers_step(answers) == CETTA_ROW )
        printf("stored: %s\n", cetta_answers_text(answers));
      cetta_answers_free(answers);
    }
    cetta_space_free(kb);
  }

  cetta_close(m);
  return 0;
}
