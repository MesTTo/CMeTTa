/* Purpose: publish C functions to MeTTa three ways, so a program written in
 *   the language can call code written here.
 * Guarantees: prints what each door answered and exits 0, or names the
 *   failure on stderr and exits 1.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#include <cetta.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* 1. A named function. `(hypot 3 4)` in MeTTa reaches this. */
static cetta_status_t op_hypot(cetta_call_t *call, void *user)
{ double a = 0, b = 0;
  (void)user;
  if ( cetta_float_value(cetta_call_arg(call, 0), &a) != CETTA_OK ||
       cetta_float_value(cetta_call_arg(call, 1), &b) != CETTA_OK )
  { cetta_call_error(call, "hypot wants two floats");
    return CETTA_ERROR;
  }
  return cetta_call_return(call, cetta_float(hypot(a, b)));
}

/* 2. A name C spells with underscores. It publishes as `word-count`, because
      each host reaches the meaning through its own casing convention. */
static cetta_status_t op_word_count(cetta_call_t *call, void *user)
{ const char *text = cetta_name(cetta_call_arg(call, 0));
  int64_t words = 0;
  bool inside = false;
  (void)user;

  if ( !text )
  { cetta_call_error(call, "word_count wants a String");
    return CETTA_ERROR;
  }
  for (; *text; text++)
  { bool space = (*text == ' ' || *text == '\t' || *text == '\n');
    if ( !space && !inside ) { words++; inside = true; }
    else if ( space ) inside = false;
  }
  return cetta_call_return(call, cetta_int(words));
}

/* 3. A C value the language carries without ever serialising it. */
typedef struct { double total; } account_t;

static cetta_status_t op_deposit(cetta_call_t *call, void *user)
{ const cetta_atom_t *handle = cetta_call_arg(call, 0);
  account_t *account;
  double amount = 0;
  (void)user;

  if ( cetta_kind(handle) != CETTA_OBJECT ||
       strcmp(cetta_object_type(handle), "account") != 0 )
  { cetta_call_error(call, "deposit wants an account handle");
    return CETTA_ERROR;
  }
  if ( cetta_float_value(cetta_call_arg(call, 1), &amount) != CETTA_OK )
  { cetta_call_error(call, "deposit wants an amount");
    return CETTA_ERROR;
  }
  account = cetta_object_value(handle);
  account->total += amount;
  return cetta_call_return(call, cetta_float(account->total));
}

static void drain(const char *label, cetta_answers_t *answers)
{ if ( !answers )
  { fprintf(stderr, "%s: %s\n", label, cetta_errmsg());
    return;
  }
  while ( cetta_answers_step(answers) == CETTA_ROW )
    printf("%s -> %s\n", label, cetta_answers_text(answers));
  cetta_answers_free(answers);
}

int main(void)
{ cetta_t *m;
  cetta_answers_t *answers = NULL;
  static account_t account = {0};
  cetta_atom_t *handle;

  if ( cetta_open(NULL, &m) != CETTA_OK )
  { fprintf(stderr, "boot: %s\n", cetta_errmsg());
    return 1;
  }

  /* Each publication names its effect class. It is required, not advisory:
     the engine reasons about caching and reordering from it. */
  cetta_op(m, "hypot", 2, CETTA_PURE_STRUCTURAL, op_hypot, NULL);
  cetta_op(m, "word_count", 1, CETTA_PURE_STRUCTURAL, op_word_count, NULL);
  cetta_op(m, "deposit", 2, CETTA_WRITES_STATE, op_deposit, NULL);

  cetta_run(m, "!(hypot 3.0 4.0)\n", &answers);
  drain("hypot", answers);

  answers = NULL;
  cetta_run(m, "!(word-count \"the quick brown fox\")\n", &answers);
  drain("word-count (published from word_count)", answers);

  /* The account never becomes text. MeTTa holds the reference and hands it
     back to deposit unchanged. */
  handle = cetta_object(&account, "account", NULL);
  { cetta_atom_t *goal = cetta_expr(3, cetta_sym("deposit"),
                                    cetta_retain(handle), cetta_float(25.0));
    answers = NULL;
    cetta_eval(cetta_self(m), goal, &answers);
    drain("deposit 25", answers);
    cetta_release(goal);

    goal = cetta_expr(3, cetta_sym("deposit"),
                      cetta_retain(handle), cetta_float(17.5));
    answers = NULL;
    cetta_eval(cetta_self(m), goal, &answers);
    drain("deposit 17.5", answers);
    cetta_release(goal);
  }
  printf("the C struct itself now holds %.2f\n", account.total);
  cetta_release(handle);

  /* A refusal from C reaches the caller as an engine error, not a wrong
     answer. */
  answers = NULL;
  if ( cetta_run(m, "!(hypot \"three\" 4.0)\n", &answers) != CETTA_OK )
    printf("refused, as it should be: %s\n", cetta_errmsg());
  cetta_answers_free(answers);

  cetta_close(m);
  return 0;
}
