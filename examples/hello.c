/* Purpose: the shortest complete C program that runs MeTTa. Boot, run a
 *   program, read its answers, shut down.
 * Guarantees: prints one line per answer and exits 0, or names the failure on
 *   stderr and exits 1.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#include <cetta.h>
#include <stdio.h>

int main(void)
{ cetta_t *m;
  cetta_answers_t *answers;
  cetta_status_t status;

  if ( cetta_open(NULL, &m) != CETTA_OK )
  { fprintf(stderr, "boot: %s\n", cetta_errmsg());
    return 1;
  }

  if ( cetta_run(m,
                 "(= (double $x) (* 2 $x))\n"
                 "!(double 21)\n"
                 "!(superpose (a b c))\n",
                 &answers) != CETTA_OK )
  { fprintf(stderr, "run: %s\n", cetta_errmsg());
    cetta_close(m);
    return 1;
  }

  while ( (status = cetta_answers_step(answers)) == CETTA_ROW )
  { const cetta_atom_t *a = cetta_answers_atom(answers);
    printf("group %zu: %s  (%s)\n",
           cetta_answers_group(answers),
           cetta_answers_text(answers),
           cetta_kind_str(cetta_kind(a)));
  }
  if ( status == CETTA_ERROR )
    fprintf(stderr, "step: %s\n", cetta_errmsg());

  cetta_answers_free(answers);

  /* An atom built in C, evaluated lazily: (+ 1 2). Nothing here parses text. */
  { cetta_atom_t *goal = cetta_expr(3, cetta_sym("+"), cetta_int(1), cetta_int(2));
    cetta_answers_t *one;
    if ( cetta_eval(cetta_self(m), goal, &one) == CETTA_OK )
    { while ( cetta_answers_step(one) == CETTA_ROW )
      { int64_t value = 0;
        cetta_int_value(cetta_answers_atom(one), &value);
        printf("built in C: (+ 1 2) = %lld\n", (long long)value);
      }
      cetta_answers_free(one);
    } else
      fprintf(stderr, "eval: %s\n", cetta_errmsg());
    cetta_release(goal);
  }

  cetta_close(m);
  return 0;
}
