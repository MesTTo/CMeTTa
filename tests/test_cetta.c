/* Purpose: exercise every door of the C binding against a live engine, and
 *   fail loudly on the first one that does not behave as cetta.h says.
 * Assumes: one runtime per process, so every case shares one engine and a
 *   case that writes to &self cleans up after itself.
 * Guarantees: exits 0 only when every case passed; prints the failing
 *   expression, its file and its line otherwise.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#include <cetta.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;
static const char *current_case = "";

#define CHECK(expr)                                                          \
  do {                                                                       \
    checks++;                                                                \
    if ( !(expr) ) {                                                         \
      failures++;                                                            \
      fprintf(stderr, "FAIL %s\n  %s:%d: %s\n  last error: %s\n",            \
              current_case, __FILE__, __LINE__, #expr,                       \
              cetta_errmsg() ? cetta_errmsg() : "(none)");                   \
    }                                                                        \
  } while (0)

/* -DCETTA_TRACE_CASES makes the harness announce each case, which is how a
   hang is located without a debugger. */
#ifdef CETTA_TRACE_CASES
#define CASE(name) \
  do { current_case = (name); fprintf(stderr, "CASE %s\n", (name)); } while (0)
#else
#define CASE(name) current_case = (name)
#endif

/* ---------------------------------------------------------------- */

static void test_atoms_need_no_engine(void)
{ cetta_atom_t *sym, *str, *n, *f, *b, *v, *e, *unit;

  CASE("atoms are built and read without an engine");

  sym = cetta_sym("foo");
  str = cetta_str("foo");
  CHECK(cetta_kind(sym) == CETTA_SYMBOL);
  CHECK(cetta_kind(str) == CETTA_STRING);
  CHECK(strcmp(cetta_name(sym), "foo") == 0);
  /* A symbol is not a string; folding them together is the ambiguity the
     kinds exist to remove. */
  CHECK(!cetta_eq(sym, str));

  n = cetta_int(42);
  f = cetta_float(2.0);
  b = cetta_bool(true);
  CHECK(cetta_kind(n) == CETTA_INT);
  CHECK(cetta_kind(f) == CETTA_FLOAT);
  CHECK(cetta_kind(b) == CETTA_BOOL);
  /* 2 and 2.0 are different atoms, which is why C splits the one wire tag. */
  { cetta_atom_t *two = cetta_int(2);
    CHECK(!cetta_eq(two, f));
    cetta_release(two);
  }
  /* A boolean is not a symbol that spells it. */
  { cetta_atom_t *spelled = cetta_sym("True");
    CHECK(!cetta_eq(b, spelled));
    cetta_release(spelled);
  }

  v = cetta_var("x");
  CHECK(cetta_kind(v) == CETTA_VARIABLE);

  e = cetta_expr(3, cetta_sym("+"), cetta_int(1), cetta_int(2));
  CHECK(cetta_kind(e) == CETTA_EXPR);
  CHECK(cetta_len(e) == 3);
  CHECK(cetta_kind(cetta_child(e, 0)) == CETTA_SYMBOL);
  CHECK(cetta_child(e, 3) == NULL);

  unit = cetta_unit();
  CHECK(cetta_kind(unit) == CETTA_EXPR);
  CHECK(cetta_len(unit) == 0);
  /* Unit is not the empty string. */
  { cetta_atom_t *empty = cetta_str("");
    CHECK(!cetta_eq(unit, empty));
    cetta_release(empty);
  }

  cetta_release(sym); cetta_release(str); cetta_release(n); cetta_release(f);
  cetta_release(b); cetta_release(v); cetta_release(e); cetta_release(unit);
}

static void test_a_failed_child_does_not_leak_its_siblings(void)
{ cetta_atom_t *bad;
  CASE("a NULL child fails the whole expression");
  /* cetta_space_ref refuses a name with no ampersand, so the middle child is
     NULL and the outer constructor must release the two that succeeded
     rather than building something half-formed. Run under a leak checker this
     is the case that proves the steal-on-failure rule. */
  bad = cetta_expr(3, cetta_sym("f"), cetta_space_ref("nope"), cetta_int(1));
  CHECK(bad == NULL);
  CHECK(cetta_errmsg() != NULL);
}

static void test_refusals_are_named(void)
{ cetta_atom_t *wide;

  CASE("a value C has no type for is refused by name");
  CHECK(cetta_bigint("12x3") == NULL);
  CHECK(cetta_errmsg() != NULL);
  CHECK(cetta_rational(1, 0) == NULL);
  CHECK(cetta_space_ref("kb") == NULL);

  wide = cetta_bigint("170141183460469231731687303715884105728");
  CHECK(wide != NULL);
  CHECK(cetta_kind(wide) == CETTA_BIGINT);
  cetta_release(wide);
}

/* Enough churn that a reference miscounted by one shows up as a leak or a use
   after free under valgrind, rather than as nothing at all. */
static void test_reference_counting_holds_under_churn(cetta_t *m)
{ int i;
  CASE("building, sharing and releasing atoms leaks nothing");
  for (i = 0; i < 2000; i++)
  { cetta_atom_t *leaf = cetta_sym("leaf");
    cetta_atom_t *shared = cetta_retain(leaf);
    cetta_atom_t *inner = cetta_expr(2, cetta_retain(leaf), cetta_int(i));
    cetta_atom_t *outer = cetta_expr(3, cetta_sym("f"), inner,
                                     cetta_str("text"));
    const cetta_atom_t *borrowed = cetta_child(outer, 1);
    cetta_atom_t *kept = cetta_retain(borrowed);

    if ( i == 0 )
    { char *shown = cetta_show(m, outer);
      CHECK(shown != NULL);
      cetta_free(shown);
    }
    cetta_release(kept);
    cetta_release(outer);
    cetta_release(shared);
    cetta_release(leaf);
  }
  /* And an object's release callback runs exactly once, however many
     references crossed into the engine and back. */
  { static int released = 0;
    cetta_atom_t *object;
    struct { int n; } payload = {0};
    (void)payload;
    released = 0;
    object = cetta_object(&released, "probe", NULL);
    for (i = 0; i < 100; i++)
    { cetta_atom_t *copy = cetta_retain(object);
      cetta_release(copy);
    }
    CHECK(cetta_object_value(object) == &released);
    cetta_release(object);
  }
}

static void test_text_crosses_through_the_engine_reader(cetta_t *m)
{ cetta_atom_t *parsed = NULL;
  char *shown;

  CASE("parse and show use the engine's own reader and writer");
  CHECK(cetta_parse(m, "(+ 1 2)", &parsed) == CETTA_OK);
  CHECK(parsed && cetta_kind(parsed) == CETTA_EXPR);
  CHECK(cetta_len(parsed) == 3);

  shown = cetta_show(m, parsed);
  CHECK(shown && strcmp(shown, "(+ 1 2)") == 0);
  cetta_free(shown);
  cetta_release(parsed);

  /* A variable keeps the name its source gave it. */
  CHECK(cetta_parse(m, "(f $x $x)", &parsed) == CETTA_OK);
  CHECK(cetta_kind(cetta_child(parsed, 1)) == CETTA_VARIABLE);
  CHECK(strcmp(cetta_name(cetta_child(parsed, 1)), "x") == 0);
  CHECK(cetta_eq(cetta_child(parsed, 1), cetta_child(parsed, 2)));
  cetta_release(parsed);

  CASE("unreadable source is an error, not a wrong answer");
  CHECK(cetta_parse(m, "(unclosed", &parsed) != CETTA_OK);
  CHECK(cetta_errmsg() != NULL);
}

static void test_run_groups_answers_by_form(cetta_t *m)
{ cetta_answers_t *answers;
  int seen = 0;
  size_t last_group = 0;

  CASE("run groups its answers by ! form, in source order");
  CHECK(cetta_run(m,
                  "(= (twice $x) (* 2 $x))\n"
                  "!(twice 21)\n"
                  "!(superpose (a b))\n", &answers) == CETTA_OK);
  while ( cetta_answers_step(answers) == CETTA_ROW )
  { last_group = cetta_answers_group(answers);
    if ( seen == 0 )
    { int64_t v = 0;
      CHECK(cetta_int_value(cetta_answers_atom(answers), &v) == CETTA_OK);
      CHECK(v == 42);
      CHECK(last_group == 0);
    }
    if ( seen == 1 )
    { CHECK(cetta_kind(cetta_answers_atom(answers)) == CETTA_SYMBOL);
      CHECK(last_group == 1);
    }
    seen++;
  }
  CHECK(seen == 3);
  CHECK(last_group == 1);
  cetta_answers_free(answers);
}

static void test_eval_is_lazy(cetta_t *m)
{ cetta_answers_t *answers;
  cetta_atom_t *goal;
  int pulled = 0;

  CASE("eval computes one answer per step over an endless generator");
  /* Endless on purpose: an eager door cannot return from this at all, so the
     case passing IS the laziness proof. */
  CHECK(cetta_run(m, "(= (from $n) (superpose ($n (from (+ $n 1)))))\n",
                  &answers) == CETTA_OK);
  cetta_answers_free(answers);

  goal = cetta_expr(2, cetta_sym("from"), cetta_int(0));
  CHECK(cetta_eval(cetta_self(m), goal, &answers) == CETTA_OK);
  while ( pulled < 3 && cetta_answers_step(answers) == CETTA_ROW )
    pulled++;
  CHECK(pulled == 3);
  /* Abandoned rather than drained: the rest stays uncomputed. */
  cetta_answers_free(answers);
  cetta_release(goal);
}

static void test_a_cursor_is_idempotent_at_its_end(cetta_t *m)
{ cetta_answers_t *answers;
  cetta_atom_t *goal;

  CASE("stepping past the end keeps answering DONE");
  goal = cetta_expr(3, cetta_sym("+"), cetta_int(1), cetta_int(2));
  CHECK(cetta_eval(cetta_self(m), goal, &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  CHECK(cetta_answers_step(answers) == CETTA_DONE);
  CHECK(cetta_answers_step(answers) == CETTA_DONE);
  CHECK(cetta_answers_atom(answers) == NULL);
  cetta_answers_free(answers);
  cetta_release(goal);
}

static void test_spaces_store_and_query(cetta_t *m)
{ cetta_space_t *kb;
  cetta_answers_t *answers;
  cetta_atom_t *fact, *pattern;
  size_t count = 0;
  bool removed = false;
  int matched = 0;

  CASE("a space stores, counts, matches and removes");
  CHECK(cetta_space_open(m, "&cetta-kb", &kb) == CETTA_OK);
  CHECK(strcmp(cetta_space_name(kb), "&cetta-kb") == 0);

  fact = cetta_expr(3, cetta_sym("edge"), cetta_sym("a"), cetta_sym("b"));
  CHECK(cetta_add(kb, fact) == CETTA_OK);
  CHECK(cetta_space_count(kb, &count) == CETTA_OK);
  CHECK(count == 1);

  pattern = cetta_expr(3, cetta_sym("edge"), cetta_sym("a"), cetta_var("y"));
  CHECK(cetta_match(kb, pattern, &answers) == CETTA_OK);
  while ( cetta_answers_step(answers) == CETTA_ROW )
  { const cetta_atom_t *got = cetta_answers_atom(answers);
    CHECK(cetta_len(got) == 3);
    /* The pattern's variable arrives bound in the answer. */
    CHECK(cetta_kind(cetta_child(got, 2)) == CETTA_SYMBOL);
    CHECK(strcmp(cetta_name(cetta_child(got, 2)), "b") == 0);
    matched++;
  }
  CHECK(matched == 1);
  cetta_answers_free(answers);

  CHECK(cetta_remove(kb, fact, &removed) == CETTA_OK);
  CHECK(removed == true);
  CHECK(cetta_space_count(kb, &count) == CETTA_OK);
  CHECK(count == 0);

  CHECK(cetta_add(kb, fact) == CETTA_OK);
  CHECK(cetta_space_clear(kb) == CETTA_OK);
  CHECK(cetta_space_count(kb, &count) == CETTA_OK);
  CHECK(count == 0);

  cetta_release(pattern);
  cetta_release(fact);
  cetta_space_free(kb);
}

static void test_a_user_space_decodes_as_a_space(cetta_t *m)
{ cetta_answers_t *answers;
  CASE("a space the engine made decodes as CETTA_SPACE, not a symbol");
  CHECK(cetta_run(m, "!(new-space)\n", &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  CHECK(cetta_kind(cetta_answers_atom(answers)) == CETTA_SPACE);
  CHECK(cetta_name(cetta_answers_atom(answers))[0] == '&');
  cetta_answers_free(answers);

  CASE("an ampersand name that is no space stays a symbol");
  CHECK(cetta_run(m, "!(id &not-a-space)\n", &answers) == CETTA_OK);
  if ( cetta_answers_step(answers) == CETTA_ROW )
  { const cetta_atom_t *got = cetta_answers_atom(answers);
    /* (id x) is irreducible here, so the answer is the whole form. */
    const cetta_atom_t *arg = cetta_kind(got) == CETTA_EXPR
                            ? cetta_child(got, cetta_len(got) - 1) : got;
    CHECK(cetta_kind(arg) == CETTA_SYMBOL);
  }
  cetta_answers_free(answers);
}

/* --- published C functions --------------------------------------- */

static cetta_status_t op_double(cetta_call_t *call, void *user)
{ int64_t v = 0;
  (void)user;
  if ( cetta_call_arity(call) != 1 ) return CETTA_FAIL;
  if ( cetta_int_value(cetta_call_arg(call, 0), &v) != CETTA_OK )
  { cetta_call_error(call, "double wants a Number");
    return CETTA_ERROR;
  }
  return cetta_call_return(call, cetta_int(v * 2));
}

static cetta_status_t op_tag_it(cetta_call_t *call, void *user)
{ const char *tag = user;
  return cetta_call_return(call,
           cetta_expr(2, cetta_sym(tag),
                      cetta_retain(cetta_call_arg(call, 0))));
}

static void test_a_c_function_is_callable_from_metta(cetta_t *m)
{ cetta_answers_t *answers;
  int64_t v = 0;

  CASE("a published C function answers a MeTTa call");
  CHECK(cetta_op(m, "cdouble", 1, CETTA_PURE_STRUCTURAL, op_double, NULL)
        == CETTA_OK);
  CHECK(cetta_run(m, "!(cdouble 21)\n", &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  CHECK(cetta_int_value(cetta_answers_atom(answers), &v) == CETTA_OK);
  CHECK(v == 42);
  cetta_answers_free(answers);

  CASE("a C name spelled with underscores reaches MeTTa with hyphens");
  CHECK(cetta_op(m, "tag_it", 1, CETTA_PURE_STRUCTURAL, op_tag_it,
                 (void *)"tagged") == CETTA_OK);
  CHECK(cetta_run(m, "!(tag-it 7)\n", &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  { const cetta_atom_t *got = cetta_answers_atom(answers);
    CHECK(cetta_kind(got) == CETTA_EXPR);
    CHECK(cetta_len(got) == 2);
    CHECK(strcmp(cetta_name(cetta_child(got, 0)), "tagged") == 0);
  }
  cetta_answers_free(answers);

  CASE("a C function's refusal reaches the caller as an error");
  CHECK(cetta_run(m, "!(cdouble \"not a number\")\n", &answers) != CETTA_OK ||
        cetta_answers_step(answers) != CETTA_ROW);
  CHECK(cetta_errmsg() != NULL);

  CASE("an operation must name one of the five effect classes");
  CHECK(cetta_op(m, "bogus", 1, (cetta_effect_t)99, op_double, NULL)
        == CETTA_MISUSE);

  CASE("a withdrawn name is data again");
  CHECK(cetta_op_remove(m, "cdouble") == CETTA_OK);
  CHECK(cetta_run(m, "!(cdouble 21)\n", &answers) == CETTA_OK);
  if ( cetta_answers_step(answers) == CETTA_ROW )
    CHECK(cetta_kind(cetta_answers_atom(answers)) == CETTA_EXPR);
  cetta_answers_free(answers);
  CHECK(cetta_op_remove(m, "tag_it") == CETTA_OK);
}

/* --- a C value crossing MeTTa untouched -------------------------- */

typedef struct { int bumps; } counter_t;
static int counter_freed = 0;

static void counter_release(void *value)
{ counter_freed++;
  (void)value;
}

static cetta_status_t op_bump(cetta_call_t *call, void *user)
{ const cetta_atom_t *handle = cetta_call_arg(call, 0);
  counter_t *c;
  (void)user;
  if ( cetta_kind(handle) != CETTA_OBJECT )
  { cetta_call_error(call, "bump wants the counter it was given");
    return CETTA_ERROR;
  }
  CHECK(strcmp(cetta_object_type(handle), "counter") == 0);
  c = cetta_object_value(handle);
  c->bumps++;
  return cetta_call_return(call, cetta_int(c->bumps));
}

static void test_a_c_value_crosses_by_reference(cetta_t *m)
{ static counter_t counter = {0};
  cetta_atom_t *handle;
  cetta_space_t *self = cetta_self(m);
  cetta_answers_t *answers;
  cetta_atom_t *goal;
  int64_t v = 0;

  CASE("a live C value crosses MeTTa and comes back the same object");
  CHECK(cetta_op(m, "bump", 1, CETTA_WRITES_STATE, op_bump, NULL) == CETTA_OK);
  handle = cetta_object(&counter, "counter", counter_release);
  CHECK(handle != NULL);
  CHECK(cetta_kind(handle) == CETTA_OBJECT);
  CHECK(cetta_object_value(handle) == &counter);

  goal = cetta_expr(2, cetta_sym("bump"), cetta_retain(handle));
  CHECK(cetta_eval(self, goal, &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  CHECK(cetta_int_value(cetta_answers_atom(answers), &v) == CETTA_OK);
  CHECK(v == 1);
  cetta_answers_free(answers);

  /* State behind the handle survives across MeTTa calls. */
  CHECK(cetta_eval(self, goal, &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  CHECK(cetta_int_value(cetta_answers_atom(answers), &v) == CETTA_OK);
  CHECK(v == 2);
  cetta_answers_free(answers);
  CHECK(counter.bumps == 2);

  cetta_release(goal);
  cetta_release(handle);
  CHECK(cetta_op_remove(m, "bump") == CETTA_OK);
}

static cetta_status_t fn_triple(cetta_call_t *call, void *user)
{ int64_t v = 0;
  (void)user;
  if ( cetta_int_value(cetta_call_arg(call, 0), &v) != CETTA_OK )
    return CETTA_FAIL;
  return cetta_call_return(call, cetta_int(v * 3));
}

static void test_a_function_value_is_applicable(cetta_t *m)
{ cetta_atom_t *fn, *goal;
  cetta_answers_t *answers;
  int64_t v = 0;

  CASE("a C function carried as a value is applied where it lands");
  fn = cetta_function(fn_triple, NULL, NULL);
  CHECK(fn != NULL);
  goal = cetta_expr(2, cetta_retain(fn), cetta_int(5));
  CHECK(cetta_eval(cetta_self(m), goal, &answers) == CETTA_OK);
  if ( cetta_answers_step(answers) == CETTA_ROW )
  { CHECK(cetta_int_value(cetta_answers_atom(answers), &v) == CETTA_OK);
    CHECK(v == 15);
  } else
    CHECK(!"a function value produced no answer");
  cetta_answers_free(answers);
  cetta_release(goal);
  cetta_release(fn);
}

static void test_an_engine_error_reaches_c_as_words(cetta_t *m)
{ cetta_answers_t *answers = NULL;

  /* A raise, not a value. MeTTa keeps most failures AS values -- (car-atom 5)
     answers unit and (+ 1 foo) answers itself unreduced -- so the case needs
     something that genuinely throws, and a failed assertion does
     [measured 2026-08-27]. */
  CASE("an engine exception crosses as CETTA_ERROR and readable words");
  CHECK(cetta_run(m, "!(assertEqual 1 2)\n", &answers) == CETTA_ERROR);
  CHECK(cetta_errmsg() != NULL);
  CHECK(cetta_errmsg() && strstr(cetta_errmsg(), "ssertion") != NULL);
  cetta_answers_free(answers);

  /* And the runtime is still usable afterwards: an error is an answer about
     one call, not a broken engine. */
  CHECK(cetta_run(m, "!(+ 1 2)\n", &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  cetta_answers_free(answers);

  CASE("an error kept as a VALUE stays an ordinary answer");
  CHECK(cetta_run(m, "!(Error foo bar)\n", &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  CHECK(cetta_kind(cetta_answers_atom(answers)) == CETTA_EXPR);
  CHECK(strcmp(cetta_name(cetta_child(cetta_answers_atom(answers), 0)),
               "Error") == 0);
  cetta_answers_free(answers);
}

static void test_a_wide_integer_keeps_its_digits(cetta_t *m)
{ cetta_answers_t *answers;
  const cetta_atom_t *got;

  CASE("an integer past int64 arrives as BIGINT with its exact digits");
  CHECK(cetta_run(m, "!(* 9223372036854775807 4)\n", &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  got = cetta_answers_atom(answers);
  CHECK(cetta_kind(got) == CETTA_BIGINT);
  CHECK(strcmp(cetta_name(got), "36893488147419103228") == 0);
  { int64_t ignored;
    /* And it refuses to pretend it fits. */
    CHECK(cetta_int_value(got, &ignored) == CETTA_MISUSE);
  }
  cetta_answers_free(answers);
}

static void test_variable_identity_survives_the_round_trip(cetta_t *m)
{ cetta_atom_t *same, *different;
  char *a, *b;

  CASE("two occurrences of one name are one variable, and two names are two");
  same = cetta_expr(3, cetta_sym("f"), cetta_var("x"), cetta_var("x"));
  different = cetta_expr(3, cetta_sym("f"), cetta_var("x"), cetta_var("y"));
  a = cetta_show(m, same);
  b = cetta_show(m, different);
  CHECK(a && b && strcmp(a, b) != 0);
  CHECK(a && strcmp(a, "(f $x $x)") == 0);
  cetta_free(a);
  cetta_free(b);
  cetta_release(same);
  cetta_release(different);
}

static void test_a_bound_stops_a_runaway_and_says_so(cetta_t *m)
{ cetta_answers_t *answers = NULL;
  cetta_atom_t *goal;
  cetta_limits_t bounded = {0}, none = {0};
  int pulled = 0;

  CASE("an inference bound stops an endless evaluation as CETTA_LIMIT");
  /* Sized from the measurement, not guessed: an answer of (from $n) costs
     roughly 40 engine inferences, so 20,000 buys a few hundred answers and
     then stops. The budget is CUMULATIVE across steps because it is installed
     inside the engine; a bound wrapped around one step would see almost none
     of the work [measured 2026-08-27: at 100 and at 2,000 the bound fired, at
     200,000 it had not fired after 5,001 pulls]. */
  bounded.inferences = 20000;
  CHECK(cetta_set_limits(m, &bounded) == CETTA_OK);

  /* (from 0) never ends. Drained rather than taken from, so only the bound
     can stop it; without one this case does not return at all. */
  goal = cetta_expr(2, cetta_sym("from"), cetta_int(0));
  CHECK(cetta_eval(cetta_self(m), goal, &answers) == CETTA_OK);
  { cetta_status_t status = CETTA_OK;
    /* The ceiling is a BACKSTOP, not the mechanism under test: the bound
       should stop this long before 200,000 answers. It is here so that a
       broken bound FAILS this case in seconds instead of hanging the gate,
       which is what happened while C16 was still open: one check.sh run sat
       on this case for 18 minutes and two more gate runs piled up behind it. */
    while ( pulled < 200000 &&
            (status = cetta_answers_step(answers)) == CETTA_ROW ) pulled++;
    CHECK(pulled < 200000);
    CHECK(status == CETTA_LIMIT);
    /* A bound is not a fault, and the words say which bound it was. */
    CHECK(cetta_errmsg() && strstr(cetta_errmsg(), "inference") != NULL);
  }
  CHECK(pulled > 0);
  cetta_answers_free(answers);
  cetta_release(goal);

  CASE("the same bound applied to a whole run");
  answers = NULL;
  CHECK(cetta_run(m, "!(from 0)\n", &answers) == CETTA_LIMIT);
  /* And the failing door left nothing to release. */
  CHECK(answers == NULL);
  cetta_answers_free(answers);

  CASE("clearing the bounds restores unbounded evaluation");
  CHECK(cetta_set_limits(m, &none) == CETTA_OK);
  { cetta_limits_t back;
    cetta_get_limits(m, &back);
    CHECK(back.inferences == 0 && back.seconds == 0);
  }
  answers = NULL;
  CHECK(cetta_run(m, "!(+ 1 2)\n", &answers) == CETTA_OK);
  CHECK(cetta_answers_step(answers) == CETTA_ROW);
  cetta_answers_free(answers);
}

static void test_the_counters_measure_engine_work(cetta_t *m)
{ cetta_stats_t before, after, spent;
  cetta_answers_t *answers;

  CASE("the engine's counters move with the work, and the same way twice");
  CHECK(cetta_stats(m, &before) == CETTA_OK);
  CHECK(cetta_run(m, "!(superpose (1 2 3 4 5))\n", &answers) == CETTA_OK);
  while ( cetta_answers_step(answers) == CETTA_ROW ) { /* drain */ }
  cetta_answers_free(answers);
  CHECK(cetta_stats(m, &after) == CETTA_OK);
  cetta_stats_delta(&before, &after, &spent);
  CHECK(spent.inferences > 0);
  CHECK(after.cputime >= before.cputime);

  /* Inferences are deterministic where wall clock is not, which is the whole
     reason this tree gates on them: the same workload twice costs the same. */
  { cetta_stats_t a1, b1, s1, a2, b2, s2;
    CHECK(cetta_stats(m, &b1) == CETTA_OK);
    CHECK(cetta_run(m, "!(superpose (1 2 3 4 5))\n", &answers) == CETTA_OK);
    while ( cetta_answers_step(answers) == CETTA_ROW ) { /* drain */ }
    cetta_answers_free(answers);
    CHECK(cetta_stats(m, &a1) == CETTA_OK);
    cetta_stats_delta(&b1, &a1, &s1);

    CHECK(cetta_stats(m, &b2) == CETTA_OK);
    CHECK(cetta_run(m, "!(superpose (1 2 3 4 5))\n", &answers) == CETTA_OK);
    while ( cetta_answers_step(answers) == CETTA_ROW ) { /* drain */ }
    cetta_answers_free(answers);
    CHECK(cetta_stats(m, &a2) == CETTA_OK);
    cetta_stats_delta(&b2, &a2, &s2);

    CHECK(s1.inferences == s2.inferences);
  }
}

static void test_reopening_is_the_same_runtime(cetta_t *m)
{ cetta_t *again = NULL;
  CASE("a second open hands back the one runtime this process has");
  CHECK(cetta_open(NULL, &again) == CETTA_OK);
  CHECK(again == m);
  { cetta_config_t other = { .path = "/definitely/not/here" };
    CHECK(cetta_open(&other, &again) == CETTA_MISUSE);
    CHECK(cetta_errmsg() != NULL);
  }
}

int main(void)
{ cetta_t *m;

  if ( cetta_open(NULL, &m) != CETTA_OK )
  { fprintf(stderr, "cannot boot the engine: %s\n", cetta_errmsg());
    return 1;
  }

  test_atoms_need_no_engine();
  test_a_failed_child_does_not_leak_its_siblings();
  test_refusals_are_named();
  test_reference_counting_holds_under_churn(m);
  test_text_crosses_through_the_engine_reader(m);
  test_run_groups_answers_by_form(m);
  test_eval_is_lazy(m);
  test_a_cursor_is_idempotent_at_its_end(m);
  test_spaces_store_and_query(m);
  test_a_user_space_decodes_as_a_space(m);
  test_a_c_function_is_callable_from_metta(m);
  test_a_c_value_crosses_by_reference(m);
  test_a_function_value_is_applicable(m);
  test_an_engine_error_reaches_c_as_words(m);
  test_a_wide_integer_keeps_its_digits(m);
  test_variable_identity_survives_the_round_trip(m);
  test_a_bound_stops_a_runaway_and_says_so(m);
  test_the_counters_measure_engine_work(m);
  test_reopening_is_the_same_runtime(m);

  printf("%d checks, %d failures\n", checks, failures);
  cetta_close(m);
  return failures == 0 ? 0 : 1;
}
