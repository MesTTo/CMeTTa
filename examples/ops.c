/* Purpose: publish C functions to MeTTa three ways, so a program written in
 *   the language can call code written here.
 * Guarantees: prints what each door answered and exits 0, or names the
 *   failure on stderr and exits 1.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#define CETTA_SHORTHAND
#include <cetta.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* 1. A named function. `(hypot 3.0 4.0)` in MeTTa reaches this.

      The arguments are read first and checked once, which is ferror()'s
      shape: cetta_clear() forgets any earlier failure, each read records its
      own, and one test at the end covers them all. */
static cetta_status op_hypot(cetta_call *call, void *user)
{ double a, b;
  (void)user;

  cetta_clear();
  a = cetta_float(cetta_arg(call, 0));
  b = cetta_float(cetta_arg(call, 1));
  if ( !cetta_ok() ) return cetta_fail(call, "hypot wants two numbers");
  return cetta_answer(call, R(hypot(a, b)));
}

/* 2. A name C spells with underscores. It publishes as `word-count`, because
      each host reaches the meaning through its own casing convention. */
static cetta_status op_word_count(cetta_call *call, void *user)
{ const char *text = cetta_name(cetta_arg(call, 0));
  int64_t words = 0;
  bool inside = false;
  (void)user;

  if ( !text ) return cetta_fail(call, "word_count wants text");
  for (; *text; text++)
  { bool space = (*text == ' ' || *text == '\t' || *text == '\n');
    if ( !space && !inside ) { words++; inside = true; }
    else if ( space ) inside = false;
  }
  return cetta_answer(call, N(words));
}

/* 3. A C value the language carries without ever serialising it. */
typedef struct { double total; } account;

static cetta_status op_deposit(cetta_call *call, void *user)
{ const cetta_atom *handle = cetta_arg(call, 0);
  account *acct;
  double amount;
  (void)user;

  if ( cetta_kind_of(handle) != CETTA_OBJECT ||
       strcmp(cetta_type(handle), "account") != 0 )
    return cetta_fail(call, "deposit wants an account handle");

  cetta_clear();
  amount = cetta_float(cetta_arg(call, 1));
  if ( !cetta_ok() ) return cetta_fail(call, "deposit wants an amount");

  acct = cetta_value(handle);
  acct->total += amount;
  return cetta_answer(call, R(acct->total));
}

int main(void)
{ cetta *m = cetta_open(NULL);
  static account acct = {0};
  cetta_atom *handle;

  if ( !m ) return fprintf(stderr, "boot: %s\n", cetta_errmsg()), 1;

  /* Each publication names its effect class. It is required, not advisory:
     the engine reasons about caching and reordering from it. Designated
     initializers mean the call site says which field is which. */
  cetta_def(m, (cetta_op){ .name = "hypot", .arity = 2,
                           .effect = CETTA_PURE, .fn = op_hypot });
  cetta_def(m, (cetta_op){ .name = "word_count", .arity = 1,
                           .effect = CETTA_PURE, .fn = op_word_count });
  cetta_def(m, (cetta_op){ .name = "deposit", .arity = 2,
                           .effect = CETTA_WRITES, .fn = op_deposit });

  printf("hypot 3 4          -> %g\n",
         cetta_one_float(cetta_run(m, "!(hypot 3.0 4.0)")));
  printf("word-count         -> %lld\n",
         (long long)cetta_one_int(cetta_run(m, "!(word-count \"the quick brown fox\")")));

  /* The account never becomes text. MeTTa holds the reference and hands it
     back to deposit unchanged, so the C struct is what actually changes. */
  handle = cetta_object(&acct, "account", NULL);
  printf("deposit 25         -> %g\n",
         cetta_one_float(cetta_eval(m, E("deposit", cetta_keep(handle), 25.0))));
  printf("deposit 17.5       -> %g\n",
         cetta_one_float(cetta_eval(m, E("deposit", cetta_keep(handle), 17.5))));
  printf("the C struct holds -> %.2f\n", acct.total);
  cetta_drop(handle);

  /* A refusal from C reaches the caller as an engine error, not a wrong
     answer. */
  cetta_clear();
  cetta_answers_free(cetta_run(m, "!(hypot \"three\" 4.0)"));
  if ( !cetta_ok() ) printf("refused, as it should be: %s\n", cetta_errmsg());

  cetta_close(m);
  return 0;
}
