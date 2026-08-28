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

#define CETTA_SHORTHAND
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

/* ================================================================== *
 * Atoms, which need no engine
 * ================================================================== */

static void test_atoms_need_no_engine(void)
{ cetta_atom *sym, *text, *n, *f, *b, *v, *e, *unit;

  CASE("atoms are built and read without an engine");

  sym = S("foo");
  text = T("foo");
  CHECK(cetta_kind_of(sym) == CETTA_SYMBOL);
  CHECK(cetta_kind_of(text) == CETTA_TEXT);
  CHECK(strcmp(cetta_name(sym), "foo") == 0);
  /* A symbol is not text; folding them together is the ambiguity the kinds
     exist to remove. */
  CHECK(!cetta_eq(sym, text));

  n = N(42);
  f = R(2.0);
  b = B(true);
  CHECK(cetta_kind_of(n) == CETTA_INT);
  CHECK(cetta_kind_of(f) == CETTA_FLOAT);
  CHECK(cetta_kind_of(b) == CETTA_BOOL);
  /* 2 and 2.0 are different atoms, which is why C splits the one wire tag. */
  { cetta_atom *two = N(2);
    CHECK(!cetta_eq(two, f));
    cetta_drop(two);
  }
  /* A boolean is not a symbol that spells it. */
  { cetta_atom *spelled = S("True");
    CHECK(!cetta_eq(b, spelled));
    cetta_drop(spelled);
  }

  v = V("x");
  CHECK(cetta_kind_of(v) == CETTA_VARIABLE);

  e = E("+", 1, 2);
  CHECK(cetta_kind_of(e) == CETTA_EXPR);
  CHECK(cetta_len(e) == 3);
  CHECK(cetta_kind_of(cetta_at(e, 0)) == CETTA_SYMBOL);
  CHECK(cetta_at(e, 3) == NULL);
  CHECK(cetta_kind_of(cetta_at(e, 3)) == CETTA_NONE);

  unit = cetta_unit();
  CHECK(cetta_kind_of(unit) == CETTA_EXPR);
  CHECK(cetta_len(unit) == 0);
  /* Unit is not the empty string. */
  { cetta_atom *empty = T("");
    CHECK(!cetta_eq(unit, empty));
    cetta_drop(empty);
  }

  cetta_drop(sym); cetta_drop(text); cetta_drop(n); cetta_drop(f);
  cetta_drop(b); cetta_drop(v); cetta_drop(e); cetta_drop(unit);
}

static void test_the_builder_coerces_each_child_by_its_c_type(void)
{ cetta_atom *e, *nested;

  CASE("cetta_expr coerces by C type and counts its own arguments");
  /* No count is written anywhere, and no child names a constructor. */
  e = E("edge", "a", 1, 2.5, V("y"));
  CHECK(cetta_len(e) == 5);
  CHECK(cetta_kind_of(cetta_at(e, 0)) == CETTA_SYMBOL);   /* a bare string  */
  CHECK(cetta_kind_of(cetta_at(e, 1)) == CETTA_SYMBOL);   /* ...is a symbol */
  CHECK(cetta_kind_of(cetta_at(e, 2)) == CETTA_INT);
  CHECK(cetta_kind_of(cetta_at(e, 3)) == CETTA_FLOAT);
  CHECK(cetta_kind_of(cetta_at(e, 4)) == CETTA_VARIABLE);
  cetta_drop(e);

  CASE("an atom argument passes through, so expressions nest");
  nested = E("f", E("g", 1), T("two"));
  CHECK(cetta_len(nested) == 3);
  CHECK(cetta_kind_of(cetta_at(nested, 1)) == CETTA_EXPR);
  CHECK(cetta_kind_of(cetta_at(nested, 2)) == CETTA_TEXT);
  cetta_drop(nested);

  CASE("every integer width reaches the same Number");
  { short s = 7; long l = 7; long long ll = 7; unsigned u = 7;
    cetta_atom *a = E("f", s), *b = E("f", l), *c = E("f", ll), *d = E("f", u);
    CHECK(cetta_eq(a, b) && cetta_eq(b, c) && cetta_eq(c, d));
    cetta_drop(a); cetta_drop(b); cetta_drop(c); cetta_drop(d);
  }
}

static void test_a_failed_child_does_not_leak_its_siblings(void)
{ cetta_atom *bad;
  CASE("a NULL child fails the whole expression");
  /* cetta_spaceref refuses a name with no ampersand, so the middle child is
     NULL and the outer constructor must drop the two that succeeded rather
     than building something half-formed. Under a leak checker this is the
     case that proves the take-on-failure rule. */
  cetta_clear();
  bad = E("f", cetta_spaceref("nope"), 1);
  CHECK(bad == NULL);
  CHECK(!cetta_ok());
}

static void test_refusals_are_named(void)
{ cetta_atom *wide;

  CASE("a value C has no type for is refused by name");
  cetta_clear();
  CHECK(cetta_bigint("12x3") == NULL);
  CHECK(!cetta_ok());
  CHECK(cetta_ratio(1, 0) == NULL);
  CHECK(cetta_spaceref("kb") == NULL);

  cetta_clear();
  wide = cetta_bigint("170141183460469231731687303715884105728");
  CHECK(wide != NULL);
  CHECK(cetta_kind_of(wide) == CETTA_BIGINT);
  CHECK(cetta_ok());
  cetta_drop(wide);
}

static void test_reading_promotes_only_where_it_is_lossless(void)
{ cetta_atom *i = N(7), *f = R(2.5), *r = cetta_ratio(1, 4), *huge;

  CASE("an Int reads as a double, because nothing is lost");
  cetta_clear();
  CHECK(cetta_float(i) == 7.0);
  CHECK(cetta_ok());

  CASE("a Float does NOT read as an Int, because rounding is not reading");
  cetta_clear();
  CHECK(cetta_int(f) == 0);
  CHECK(!cetta_ok());

  CASE("a Rational reads as its quotient");
  cetta_clear();
  CHECK(cetta_float(r) == 0.25);
  CHECK(cetta_ok());

  CASE("an Int too wide for a double is refused rather than rounded");
  huge = N(9007199254740993LL);           /* 2^53 + 1 */
  cetta_clear();
  CHECK(cetta_float(huge) == 0.0);
  CHECK(!cetta_ok());
  CHECK(cetta_error() == CETTA_UNSUPPORTED);
  /* And it still reads exactly as what it is. */
  cetta_clear();
  CHECK(cetta_int(huge) == 9007199254740993LL);
  CHECK(cetta_ok());

  cetta_drop(i); cetta_drop(f); cetta_drop(r); cetta_drop(huge);
}

static void test_the_error_state_is_errno_shaped(void)
{ CASE("a failure sticks until it is cleared, so a run is checked once");
  cetta_clear();
  CHECK(cetta_ok());
  CHECK(cetta_errmsg() == NULL);
  CHECK(cetta_error() == CETTA_OK);

  cetta_drop(cetta_bigint("nope"));
  CHECK(!cetta_ok());
  CHECK(cetta_errmsg() != NULL);

  /* A success afterwards does NOT clear it, which is the whole point: three
     reads can be checked with one test. */
  cetta_drop(S("fine"));
  CHECK(!cetta_ok());

  cetta_clear();
  CHECK(cetta_ok());
}

static void test_reference_counting_holds_under_churn(void)
{ int i;
  CASE("building, sharing and dropping atoms leaks nothing");
  for (i = 0; i < 2000; i++)
  { cetta_atom *leaf = S("leaf");
    cetta_atom *shared = cetta_keep(leaf);
    cetta_atom *outer = E("f", E(cetta_keep(leaf), i), T("text"));
    const cetta_atom *borrowed = cetta_at(outer, 1);
    cetta_atom *kept = cetta_keep(borrowed);

    if ( i == 0 ) CHECK(cetta_show(outer) != NULL);
    cetta_drop(kept);
    cetta_drop(outer);
    cetta_drop(shared);
    cetta_drop(leaf);
  }
}

/* ================================================================== *
 * Text, running, and the cursor
 * ================================================================== */

static void test_text_crosses_through_the_engine_reader(void)
{ cetta_atom *parsed;

  CASE("parse and show use the engine's own reader and writer");
  parsed = cetta_parse("(+ 1 2)");
  CHECK(parsed && cetta_kind_of(parsed) == CETTA_EXPR);
  CHECK(cetta_len(parsed) == 3);
  CHECK(strcmp(cetta_show(parsed), "(+ 1 2)") == 0);
  cetta_drop(parsed);

  CASE("show hands back storage it owns, so it drops into printf");
  { cetta_atom *a = S("alpha"), *b = S("beta");
    /* Both renderings must still be readable in one call, which one slot
       could not manage. */
    const char *sa = cetta_show(a), *sb = cetta_show(b);
    CHECK(strcmp(sa, "alpha") == 0);
    CHECK(strcmp(sb, "beta") == 0);
    cetta_drop(a); cetta_drop(b);
  }

  CASE("a variable keeps the name its source gave it");
  parsed = cetta_parse("(f $x $x)");
  CHECK(cetta_kind_of(cetta_at(parsed, 1)) == CETTA_VARIABLE);
  CHECK(strcmp(cetta_name(cetta_at(parsed, 1)), "x") == 0);
  CHECK(cetta_eq(cetta_at(parsed, 1), cetta_at(parsed, 2)));
  cetta_drop(parsed);

  CASE("unreadable source is a refusal, not a wrong answer");
  cetta_clear();
  CHECK(cetta_parse("(unclosed") == NULL);
  CHECK(!cetta_ok());
}

static void test_run_groups_answers_by_form(cetta *m)
{ int seen = 0;
  size_t last_group = 0;

  CASE("run groups its answers by ! form, in source order");
  cetta_each_cursor (a, it, cetta_run(m,
        "(= (twice $x) (* 2 $x))\n"
        "!(twice 21)\n"
        "!(superpose (a b))\n"))
  { last_group = cetta_group(it);
    if ( seen == 0 )
    { CHECK(cetta_int(a) == 42);
      CHECK(last_group == 0);
    }
    if ( seen == 1 )
    { CHECK(cetta_kind_of(a) == CETTA_SYMBOL);
      CHECK(last_group == 1);
    }
    CHECK(cetta_answer_text(it) != NULL);
    seen++;
  }
  CHECK(seen == 3);
  CHECK(last_group == 1);
}

static void test_the_walk_closes_its_cursor_on_break(cetta *m)
{ int pulled = 0;

  CASE("eval computes one answer per step over an endless generator");
  /* Endless on purpose: an eager door cannot return from this at all, so the
     case passing IS the laziness proof, and `break` leaving the cursor closed
     is what makes it safe to write. */
  cetta_answers_free(cetta_run(m,
      "(= (from $n) (superpose ($n (from (+ $n 1)))))"));

  cetta_each (a, cetta_eval(m, E("from", 0)))
  { CHECK(cetta_int(a) == pulled);
    if ( ++pulled == 3 ) break;
  }
  CHECK(pulled == 3);

  CASE("two walks nest without their cursors colliding");
  { int pairs = 0;
    cetta_each (x, cetta_eval(m, E("superpose", E("a", "b"))))
    { cetta_each (y, cetta_eval(m, E("superpose", E("c", "d"))))
      { (void)x; (void)y; pairs++; }
    }
    CHECK(pairs == 4);
  }
}

static void test_one_and_first_make_different_claims(cetta *m)
{ cetta_atom *a;

  CASE("cetta_one is a claim that there is exactly one answer");
  cetta_clear();
  CHECK(cetta_one_int(cetta_eval(m, E("+", 1, 2))) == 3);
  CHECK(cetta_ok());

  CASE("cetta_one refuses a question that answered twice");
  cetta_clear();
  a = cetta_one(cetta_eval(m, E("superpose", E("a", "b"))));
  CHECK(a == NULL);
  CHECK(cetta_error() == CETTA_MISUSE);

  CASE("cetta_first takes the first and makes no such claim");
  cetta_clear();
  a = cetta_first(cetta_eval(m, E("superpose", E("a", "b"))));
  CHECK(a != NULL);
  CHECK(cetta_ok());
  cetta_drop(a);

  CASE("cetta_one on no answers at all is a recorded failure");
  cetta_clear();
  CHECK(cetta_one(cetta_eval(m, E("empty"))) == NULL);
  CHECK(!cetta_ok());

  CASE("cetta_all collects every answer as one owned array");
  { size_t n = 0;
    cetta_atom **all = cetta_all(cetta_eval(m, E("superpose", E(1, 2, 3))), &n);
    CHECK(n == 3);
    CHECK(all != NULL && cetta_int(all[0]) + cetta_int(all[1]) + cetta_int(all[2]) == 6);
    cetta_atoms_free(all, n);
  }
}

/* ================================================================== *
 * Spaces
 * ================================================================== */

static void test_spaces_store_and_query(cetta *m)
{ cetta_space *kb;
  int matched = 0;

  CASE("a space stores, counts, matches and removes");
  kb = cetta_space_open(m, "&cetta-kb");
  CHECK(kb != NULL);
  CHECK(strcmp(cetta_space_name(kb), "&cetta-kb") == 0);

  CHECK(cetta_add(kb, E("edge", "a", "b")));
  CHECK(cetta_count(kb) == 1);

  cetta_each (got, cetta_match(kb, E("edge", "a", V("y"))))
  { CHECK(cetta_len(got) == 3);
    /* The pattern's variable arrives bound in the answer. */
    CHECK(strcmp(cetta_name(cetta_at(got, 2)), "b") == 0);
    matched++;
  }
  CHECK(matched == 1);

  CHECK(cetta_del(kb, E("edge", "a", "b")) == true);
  CHECK(cetta_count(kb) == 0);
  CHECK(cetta_del(kb, E("edge", "a", "b")) == false);

  CHECK(cetta_add(kb, E("edge", "a", "b")));
  CHECK(cetta_wipe(kb));
  CHECK(cetta_count(kb) == 0);
  cetta_space_close(kb);
}

static void test_one_verb_takes_either_receiver(cetta *m)
{ cetta_space *kb;
  size_t before;

  CASE("the same verb points at a runtime or at a space");
  before = cetta_count(m);                    /* a cetta *  means &self   */
  CHECK(cetta_add(m, E("cetta-receiver-probe", 1)));
  CHECK(cetta_count(m) == before + 1);

  kb = cetta_space_open(m, "&cetta-receiver");
  CHECK(cetta_count(kb) == 0);                /* a cetta_space * means it */
  CHECK(cetta_add(kb, E("cetta-receiver-probe", 1)));
  CHECK(cetta_count(kb) == 1);
  /* The two receivers are different stores, which is the point. */
  CHECK(cetta_count(m) == before + 1);

  CHECK(cetta_del(m, E("cetta-receiver-probe", 1)));
  CHECK(cetta_wipe(kb));
  cetta_space_close(kb);
}

static void test_a_user_space_decodes_as_a_space(cetta *m)
{ CASE("a space the engine made decodes as CETTA_SPACE, not a symbol");
  cetta_each (a, cetta_run(m, "!(new-space)"))
  { CHECK(cetta_kind_of(a) == CETTA_SPACE);
    CHECK(cetta_name(a)[0] == '&');
  }

  CASE("an ampersand name that is no space stays a symbol");
  cetta_each (a, cetta_run(m, "!(id &not-a-space)"))
  { const cetta_atom *arg = cetta_kind_of(a) == CETTA_EXPR
                          ? cetta_at(a, cetta_len(a) - 1) : a;
    CHECK(cetta_kind_of(arg) == CETTA_SYMBOL);
  }
}

/* ================================================================== *
 * Published C functions
 * ================================================================== */

static cetta_status op_double(cetta_call *call, void *user)
{ int64_t v;
  (void)user;
  if ( cetta_arity(call) != 1 ) return CETTA_FAIL;
  cetta_clear();
  v = cetta_int(cetta_arg(call, 0));
  if ( !cetta_ok() ) return cetta_fail(call, "double wants a Number");
  return cetta_answer(call, N(v * 2));
}

static cetta_status op_tag_it(cetta_call *call, void *user)
{ return cetta_answer(call, E((const char *)user,
                              cetta_keep(cetta_arg(call, 0))));
}

static void test_a_c_function_is_callable_from_metta(cetta *m)
{ CASE("a published C function answers a MeTTa call");
  CHECK(cetta_def(m, (cetta_op){ .name = "cdouble", .arity = 1,
                                 .effect = CETTA_PURE, .fn = op_double }));
  CHECK(cetta_one_int(cetta_run(m, "!(cdouble 21)")) == 42);

  CASE("a C name spelled with underscores reaches MeTTa with hyphens");
  CHECK(cetta_def(m, (cetta_op){ .name = "tag_it", .arity = 1,
                                 .effect = CETTA_PURE, .fn = op_tag_it,
                                 .user = (void *)"tagged" }));
  { cetta_atom *got = cetta_one(cetta_run(m, "!(tag-it 7)"));
    CHECK(got && cetta_kind_of(got) == CETTA_EXPR);
    CHECK(cetta_len(got) == 2);
    CHECK(strcmp(cetta_name(cetta_at(got, 0)), "tagged") == 0);
    cetta_drop(got);
  }

  CASE("a C function's refusal reaches the caller as an error");
  cetta_clear();
  cetta_answers_free(cetta_run(m, "!(cdouble \"not a number\")"));
  CHECK(!cetta_ok());

  CASE("an operation must name one of the five effect classes");
  cetta_clear();
  CHECK(!cetta_def(m, (cetta_op){ .name = "bogus", .arity = 1,
                                  .effect = (cetta_effect)99, .fn = op_double }));
  CHECK(cetta_error() == CETTA_MISUSE);

  CASE("a withdrawn name is data again");
  CHECK(cetta_undef(m, "cdouble"));
  cetta_each (a, cetta_run(m, "!(cdouble 21)"))
      CHECK(cetta_kind_of(a) == CETTA_EXPR);
  CHECK(cetta_undef(m, "tag_it"));
}

typedef struct { int bumps; } counter;

static cetta_status op_bump(cetta_call *call, void *user)
{ const cetta_atom *handle = cetta_arg(call, 0);
  counter *c;
  (void)user;
  if ( cetta_kind_of(handle) != CETTA_OBJECT )
    return cetta_fail(call, "bump wants the counter it was given");
  CHECK(strcmp(cetta_type(handle), "counter") == 0);
  c = cetta_value(handle);
  c->bumps++;
  return cetta_answer(call, N(c->bumps));
}

static void test_a_c_value_crosses_by_reference(cetta *m)
{ static counter c = {0};
  cetta_atom *handle;

  CASE("a live C value crosses MeTTa and comes back the same object");
  CHECK(cetta_def(m, (cetta_op){ .name = "bump", .arity = 1,
                                 .effect = CETTA_WRITES, .fn = op_bump }));
  handle = cetta_object(&c, "counter", NULL);
  CHECK(handle != NULL);
  CHECK(cetta_kind_of(handle) == CETTA_OBJECT);
  CHECK(cetta_value(handle) == &c);

  /* State behind the handle survives across MeTTa calls. */
  CHECK(cetta_one_int(cetta_eval(m, E("bump", cetta_keep(handle)))) == 1);
  CHECK(cetta_one_int(cetta_eval(m, E("bump", cetta_keep(handle)))) == 2);
  CHECK(c.bumps == 2);

  cetta_drop(handle);
  CHECK(cetta_undef(m, "bump"));
}

static cetta_status fn_triple(cetta_call *call, void *user)
{ int64_t v;
  (void)user;
  cetta_clear();
  v = cetta_int(cetta_arg(call, 0));
  if ( !cetta_ok() ) return CETTA_FAIL;
  return cetta_answer(call, N(v * 3));
}

static void test_a_function_value_is_applicable(cetta *m)
{ cetta_atom *fn;

  CASE("a C function carried as a value is applied where it lands");
  fn = cetta_function(fn_triple, NULL, NULL);
  CHECK(fn != NULL);
  CHECK(cetta_one_int(cetta_eval(m, E(cetta_keep(fn), 5))) == 15);
  cetta_drop(fn);
}

/* ================================================================== *
 * Errors, wide values, bounds and counters
 * ================================================================== */

static void test_an_engine_error_reaches_c_as_words(cetta *m)
{ /* A raise, not a value. MeTTa keeps most failures AS values -- (car-atom 5)
     answers unit and (+ 1 foo) answers itself unreduced -- so the case needs
     something that genuinely throws, and a failed assertion does. */
  CASE("an engine exception crosses as CETTA_ERROR and readable words");
  cetta_clear();
  CHECK(cetta_run(m, "!(assertEqual 1 2)") == NULL);
  CHECK(cetta_error() == CETTA_ERROR);
  CHECK(cetta_errmsg() && strstr(cetta_errmsg(), "ssertion") != NULL);

  CASE("the runtime is still usable after one call raised");
  cetta_clear();
  CHECK(cetta_one_int(cetta_run(m, "!(+ 1 2)")) == 3);
  CHECK(cetta_ok());

  CASE("an error kept as a VALUE stays an ordinary answer");
  { cetta_atom *got = cetta_one(cetta_run(m, "!(Error foo bar)"));
    CHECK(got && cetta_kind_of(got) == CETTA_EXPR);
    CHECK(strcmp(cetta_name(cetta_at(got, 0)), "Error") == 0);
    cetta_drop(got);
  }
}

static void test_a_wide_integer_keeps_its_digits(cetta *m)
{ cetta_atom *got;

  CASE("an integer past int64 arrives as BIGINT with its exact digits");
  got = cetta_one(cetta_run(m, "!(* 9223372036854775807 4)"));
  CHECK(got && cetta_kind_of(got) == CETTA_BIGINT);
  CHECK(strcmp(cetta_name(got), "36893488147419103228") == 0);
  /* And it refuses to pretend it fits. */
  cetta_clear();
  CHECK(cetta_int(got) == 0);
  CHECK(!cetta_ok());
  cetta_drop(got);
}

static void test_variable_identity_survives_the_round_trip(void)
{ cetta_atom *same, *different;
  char *a, *b;

  CASE("two occurrences of one name are one variable, two names are two");
  same = E("f", V("x"), V("x"));
  different = E("f", V("x"), V("y"));
  a = cetta_show_dup(same);
  b = cetta_show_dup(different);
  CHECK(a && b && strcmp(a, b) != 0);
  CHECK(a && strcmp(a, "(f $x $x)") == 0);
  cetta_free(a);
  cetta_free(b);
  cetta_drop(same);
  cetta_drop(different);
}

static void test_a_bound_stops_a_runaway_and_says_so(cetta *m)
{ cetta_limits bounded = {0}, none = {0};
  int pulled = 0;

  CASE("an inference bound stops an endless evaluation as CETTA_LIMIT");
  /* Sized from the measurement, not guessed: an answer of (from $n) costs
     roughly 40 engine inferences, so 20,000 buys a few hundred answers and
     then stops. The budget is CUMULATIVE across steps. */
  bounded.inferences = 20000;
  CHECK(cetta_limit(m, &bounded));

  cetta_clear();
  /* The ceiling is a BACKSTOP: the bound should stop this long before
     200,000 answers, and it is here so a broken bound FAILS the case in
     seconds instead of hanging the gate. */
  cetta_each (a, cetta_eval(m, E("from", 0)))
  { (void)a;
    if ( ++pulled >= 200000 ) break;
  }
  CHECK(pulled > 0);
  CHECK(pulled < 200000);
  CHECK(cetta_error() == CETTA_LIMIT);
  CHECK(cetta_errmsg() && strstr(cetta_errmsg(), "inference") != NULL);

  CASE("the same bound applied to a whole run");
  cetta_clear();
  CHECK(cetta_run(m, "!(from 0)") == NULL);
  CHECK(cetta_error() == CETTA_LIMIT);

  CASE("clearing the bounds restores unbounded evaluation");
  CHECK(cetta_limit(m, &none));
  CHECK(cetta_limits_of(m).inferences == 0);
  cetta_clear();
  CHECK(cetta_one_int(cetta_run(m, "!(+ 1 2)")) == 3);
  CHECK(cetta_ok());
}

static void test_the_counters_measure_engine_work(cetta *m)
{ cetta_stats before, after, spent;

  CASE("the engine's counters move with the work, and the same way twice");
  before = cetta_stats_now(m);
  cetta_each (x, cetta_run(m, "!(superpose (1 2 3 4 5))")) (void)x;
  after = cetta_stats_now(m);
  spent = cetta_stats_since(before, after);
  CHECK(spent.inferences > 0);
  CHECK(after.cputime >= before.cputime);

  /* Inferences are deterministic where wall clock is not, which is the whole
     reason this tree gates on them: the same workload twice costs the same. */
  { cetta_stats b1, a1, b2, a2;
    b1 = cetta_stats_now(m);
    cetta_each (x, cetta_run(m, "!(superpose (1 2 3 4 5))")) (void)x;
    a1 = cetta_stats_now(m);

    b2 = cetta_stats_now(m);
    cetta_each (x, cetta_run(m, "!(superpose (1 2 3 4 5))")) (void)x;
    a2 = cetta_stats_now(m);

    CHECK(cetta_stats_since(b1, a1).inferences ==
          cetta_stats_since(b2, a2).inferences);
  }
}

static void test_verbosity_reaches_the_engines_own_door(cetta *m)
{ bool was;

  /* cetta_verbose() records the new setting only when the Prolog call came
     back CETTA_OK, so a round trip that reports back what was just set is
     proof the call ran. It is worth its own case because the predicate it
     reaches moved: bridge.pl used to define metta_c_set_silent/1 in `user`,
     and this now calls engine/filereader.pl's metta_host_set_silent/1, which
     reaches `user` by EXPORT rather than by being defined there.

     Whether the engine then keeps its diagnostics off this process's stdout
     is asserted where a test can see a file descriptor after the process has
     flushed it: the Python seat's C-binding test reads THIS binary's
     streams. */
  CASE("cetta_verbose reaches the engine's published verbosity door");
  was = cetta_verbose(m, true);
  CHECK(cetta_verbose(m, false) == true);
  CHECK(cetta_verbose(m, was) == false);
}

static void test_reopening_is_the_same_runtime(cetta *m)
{ CASE("a second open hands back the one runtime this process has");
  cetta_clear();
  CHECK(cetta_open(NULL) == m);
  CHECK(cetta_ok());
  { cetta_config other = { .path = "/definitely/not/here" };
    CHECK(cetta_open(&other) == NULL);
    CHECK(cetta_error() == CETTA_MISUSE);
    CHECK(cetta_errmsg() != NULL);
  }
}

#ifdef CETTA_HAS_AUTO
static void test_scope_cleanup_releases_on_every_exit(cetta *m)
{ CASE("CETTA_AUTO releases whatever way the block is left");
  { CETTA_AUTO cetta_atom *held = cetta_one(cetta_eval(m, E("+", 1, 1)));
    CHECK(cetta_int(held) == 2);
  } /* dropped here */

  { CETTA_AUTO_ASK cetta_answers *r = cetta_run(m, "!(superpose (1 2 3))");
    CHECK(cetta_next(r) != NULL);
  } /* closed here, with two answers still uncomputed */

  /* And a value can be handed out of such a block without being released. */
  { cetta_atom *escaped;
    { CETTA_AUTO cetta_atom *tmp = S("kept");
      escaped = CETTA_TAKE(tmp);
    }
    CHECK(escaped && strcmp(cetta_name(escaped), "kept") == 0);
    cetta_drop(escaped);
  }
}
#endif

int main(void)
{ cetta *m = cetta_open(NULL);

  if ( !m )
  { fprintf(stderr, "cannot boot the engine: %s\n", cetta_errmsg());
    return 1;
  }

  test_atoms_need_no_engine();
  test_the_builder_coerces_each_child_by_its_c_type();
  test_a_failed_child_does_not_leak_its_siblings();
  test_refusals_are_named();
  test_reading_promotes_only_where_it_is_lossless();
  test_the_error_state_is_errno_shaped();
  test_reference_counting_holds_under_churn();
  test_text_crosses_through_the_engine_reader();
  test_run_groups_answers_by_form(m);
  test_the_walk_closes_its_cursor_on_break(m);
  test_one_and_first_make_different_claims(m);
  test_spaces_store_and_query(m);
  test_one_verb_takes_either_receiver(m);
  test_a_user_space_decodes_as_a_space(m);
  test_a_c_function_is_callable_from_metta(m);
  test_a_c_value_crosses_by_reference(m);
  test_a_function_value_is_applicable(m);
  test_an_engine_error_reaches_c_as_words(m);
  test_a_wide_integer_keeps_its_digits(m);
  test_variable_identity_survives_the_round_trip();
  test_a_bound_stops_a_runaway_and_says_so(m);
  test_the_counters_measure_engine_work(m);
  test_verbosity_reaches_the_engines_own_door(m);
  test_reopening_is_the_same_runtime(m);
#ifdef CETTA_HAS_AUTO
  test_scope_cleanup_releases_on_every_exit(m);
#endif

  printf("%d checks, %d failures\n", checks, failures);
  cetta_close(m);
  return failures == 0 ? 0 : 1;
}
