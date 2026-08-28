/* Purpose: turn C source into MeTTa the engine can see into, which is a
 *   different thing from publishing a C function it can call.
 * Guarantees: prints what each door bought and exits 0, or names the failure
 *   on stderr and exits 1.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#define MT_SHORTHAND
#include <cetta.h>
#include <stdio.h>

/* --- the two doors, side by side ------------------------------------ *
 *
 * mt_def publishes a C function. The engine CALLS it, and because nothing can
 * be seen of what it does, it must declare an effect class.
 *
 * mt_lower installs an EQUATION. It is MeTTa, so the engine reads it,
 * type-checks it, specialises it, matches on it, and a call crosses into no
 * host at all.
 *
 * The preprocessor is what makes the second one possible in C. Python lowers
 * by reading a function's __code__ and Node by reading its toString(); C has
 * neither at run time, but `#` is access to the program's own source at the
 * one moment C offers it.
 */

static mt_status op_triple(mt_call *call, void *user)
{ int64_t v;
  (void)user;
  mt_clear();
  v = mt_int(mt_arg(call, 0));
  if ( !mt_ok() ) return mt_fail(call, "triple wants a Number");
  return mt_answer(call, N(v * 3));
}

/* --- one body, two languages ---------------------------------------- *
 *
 * The operators are parameters, so the same body expands to C in one mode and
 * to MeTTa tokens in the other. The function exists once and is callable from
 * both, which is what the other seats' twins buy, bought the way C buys it.
 */
#define POLY(ADD, MUL, x)  ADD(MUL(3, x), 1)
#define C_ADD(a, b)        ((a) + (b))
#define C_MUL(a, b)        ((a) * (b))
#define M_ADD(a, b)        (+ a b)
#define M_MUL(a, b)        (* a b)

static int64_t poly(int64_t x) { return POLY(C_ADD, C_MUL, x); }

int main(void)
{ metta *m = mt_open(NULL);
  if ( !m ) return fprintf(stderr, "boot: %s\n", mt_errmsg()), 1;

  /* Called: the engine crosses into C, and had to be told the effect class. */
  mt_def(m, (mt_op){ .name = "triple", .arity = 1,
                     .effect = MT_PURE, .fn = op_triple });
  printf("called   (triple 7) = %lld\n",
         (long long)mt_one_int(mt_run(m, "!(triple 7)")));

  /* Lowered: the body is C tokens the compiler saw, installed as MeTTa. No
     quoting, no escaped newlines, and unbalanced parentheses are a compile
     error rather than a runtime one. */
  mt_lower(m, (twice $x), (* 2 $x));
  mt_lower(m, (fib $n), (if (< $n 2) $n
                            (+ (fib (- $n 1)) (fib (- $n 2)))));
  printf("lowered  (twice 21) = %lld\n",
         (long long)mt_one_int(mt_run(m, "!(twice 21)")));
  printf("lowered  (fib 20)   = %lld\n",
         (long long)mt_one_int(mt_run(m, "!(fib 20)")));

  /* One body, both languages. */
  mt_lower(m, (poly $x), POLY(M_ADD, M_MUL, $x));
  printf("in MeTTa (poly 5)   = %lld\n",
         (long long)mt_one_int(mt_run(m, "!(poly 5)")));
  printf("in C     poly(5)    = %lld\n", (long long)poly(5));

  /* And the difference that matters: a lowered equation is an ATOM in the
     space, so the engine can be asked about it. A published C function is
     opaque and there is nothing to ask. */
  mt_each (a, mt_match(mt_self(m), E("=", E("poly", V("x")), V("body"))))
      printf("the engine can see: %s\n", mt_show(a));

  mt_each (a, mt_match(mt_self(m), E("=", E("triple", V("x")), V("body"))))
      printf("...but not this:    %s\n", mt_show(a));
  printf("(nothing printed above, because a called function has no equation)\n");

  mt_close(m);
  return 0;
}
