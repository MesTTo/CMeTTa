/* Purpose: publish C functions to MeTTa three ways, so a program written in
 *   the language can call code written here.
 * Guarantees: prints what each door answered and exits 0, or names the
 *   failure on stderr and exits 1.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#define MT_SHORTHAND
#include <cetta.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* 1. A named function. `(hypot 3.0 4.0)` in MeTTa reaches this.

      The arguments are read first and checked once, which is ferror()'s
      shape: mt_clear() forgets any earlier failure, each read records its
      own, and one test at the end covers them all. */
static mt_status op_hypot(mt_call *call, void *user)
{ double a, b;
  (void)user;

  mt_clear();
  a = mt_float(mt_arg(call, 0));
  b = mt_float(mt_arg(call, 1));
  if ( !mt_ok() ) return mt_fail(call, "hypot wants two numbers");
  return mt_answer(call, R(hypot(a, b)));
}

/* 2. A name C spells with underscores. It publishes as `word-count`, because
      each host reaches the meaning through its own casing convention. */
static mt_status op_word_count(mt_call *call, void *user)
{ const char *text = mt_name(mt_arg(call, 0));
  int64_t words = 0;
  bool inside = false;
  (void)user;

  if ( !text ) return mt_fail(call, "word_count wants text");
  for (; *text; text++)
  { bool space = (*text == ' ' || *text == '\t' || *text == '\n');
    if ( !space && !inside ) { words++; inside = true; }
    else if ( space ) inside = false;
  }
  return mt_answer(call, N(words));
}

/* 3. A C value the language carries without ever serialising it. */
typedef struct { double total; } account;

static mt_status op_deposit(mt_call *call, void *user)
{ const mt_atom *handle = mt_arg(call, 0);
  account *acct;
  double amount;
  (void)user;

  if ( mt_kind_of(handle) != MT_OBJECT ||
       strcmp(mt_type(handle), "account") != 0 )
    return mt_fail(call, "deposit wants an account handle");

  mt_clear();
  amount = mt_float(mt_arg(call, 1));
  if ( !mt_ok() ) return mt_fail(call, "deposit wants an amount");

  acct = mt_value(handle);
  acct->total += amount;
  return mt_answer(call, R(acct->total));
}

int main(void)
{ metta *m = mt_open(NULL);
  static account acct = {0};
  mt_atom *handle;

  if ( !m ) return fprintf(stderr, "boot: %s\n", mt_errmsg()), 1;

  /* Each publication names its effect class. It is required, not advisory:
     the engine reasons about caching and reordering from it. Designated
     initializers mean the call site says which field is which. */
  mt_def(m, (mt_op){ .name = "hypot", .arity = 2,
                           .effect = MT_PURE, .fn = op_hypot });
  mt_def(m, (mt_op){ .name = "word_count", .arity = 1,
                           .effect = MT_PURE, .fn = op_word_count });
  mt_def(m, (mt_op){ .name = "deposit", .arity = 2,
                           .effect = MT_WRITES, .fn = op_deposit });

  printf("hypot 3 4          -> %g\n",
         mt_one_float(mt_run(m, "!(hypot 3.0 4.0)")));
  printf("word-count         -> %lld\n",
         (long long)mt_one_int(mt_run(m, "!(word-count \"the quick brown fox\")")));

  /* The account never becomes text. MeTTa holds the reference and hands it
     back to deposit unchanged, so the C struct is what actually changes. */
  handle = mt_object(&acct, "account", NULL);
  printf("deposit 25         -> %g\n",
         mt_one_float(mt_eval(m, E("deposit", mt_keep(handle), 25.0))));
  printf("deposit 17.5       -> %g\n",
         mt_one_float(mt_eval(m, E("deposit", mt_keep(handle), 17.5))));
  printf("the C struct holds -> %.2f\n", acct.total);
  mt_drop(handle);

  /* A refusal from C reaches the caller as an engine error, not a wrong
     answer. */
  mt_clear();
  mt_answers_free(mt_run(m, "!(hypot \"three\" 4.0)"));
  if ( !mt_ok() ) printf("refused, as it should be: %s\n", mt_errmsg());

  mt_close(m);
  return 0;
}
