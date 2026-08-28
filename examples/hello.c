/* Purpose: the shortest complete C program that runs MeTTa, and the shape
 *   every other one starts from.
 * Guarantees: prints one line per answer and exits 0, or names the failure on
 *   stderr and exits 1.
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
  if ( !m ) return fprintf(stderr, "boot: %s\n", cetta_errmsg()), 1;

  /* Run a program. The loop opens the cursor, walks it and closes it. */
  cetta_each_cursor (a, it, cetta_run(m,
        "(= (double $x) (* 2 $x))\n"
        "!(double 21)\n"
        "!(superpose (a b c))\n"))
      printf("group %zu: %-4s %s\n",
             cetta_group(it), cetta_show(a), cetta_kind_str(cetta_kind_of(a)));

  /* A term built in C: no count, no per-child constructor, no text parsed.
     cetta_eval takes the goal and cetta_one_int closes the cursor and drops
     the answer, so the whole question is one expression that owns nothing. */
  printf("(+ 1 2) = %lld\n",
         (long long)cetta_one_int(cetta_eval(m, E("+", 1, 2))));

  /* Reading promotes where it is lossless, so an Int answers cetta_float. */
  printf("as a double: %g\n", cetta_one_float(cetta_eval(m, E("+", 1, 2))));

  /* Errors are checked where it suits you, not at every call. */
  cetta_clear();
  cetta_drop(cetta_parse("(unclosed"));
  if ( !cetta_ok() ) printf("refused, as it should be: %s\n", cetta_errmsg());

  cetta_close(m);
  return 0;
}
