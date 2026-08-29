/* Purpose: the shortest complete C program that runs MeTTa, and the shape
 *   every other one starts from.
 * Guarantees: prints one line per answer and exits 0, or names the failure on
 *   stderr and exits 1.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#define MT_SHORTHAND
#include <cmetta.h>
#include <stdio.h>

int main(void)
{ metta *m = mt_open(NULL);
  if ( !m ) return fprintf(stderr, "boot: %s\n", mt_errmsg()), 1;

  /* Run a program. The loop opens the cursor, walks it and closes it. */
  mt_rows (row, mt_run(m,
        "(= (double $x) (* 2 $x))\n"
        "!(double 21)\n"
        "!(superpose (a b c))\n"))
      printf("group %zu: %-4s %s\n",
             row->group, row->text, mt_kind_str(mt_kind_of(row->atom)));

  /* A term built in C: no count, no per-child constructor, no text parsed.
     mt_eval takes the goal and mt_one_int closes the cursor and drops
     the answer, so the whole question is one expression that owns nothing. */
  printf("(+ 1 2) = %lld\n",
         (long long)mt_one_int(mt_eval(m, E("+", 1, 2))));

  /* Reading promotes where it is lossless, so an Int answers mt_float. */
  printf("as a double: %g\n", mt_one_float(mt_eval(m, E("+", 1, 2))));

  /* Errors are checked where it suits you, not at every call. */
  mt_clear();
  mt_drop(mt_parse("(unclosed"));
  if ( !mt_ok() ) printf("refused, as it should be: %s\n", mt_errmsg());

  mt_close(m);
  return 0;
}
