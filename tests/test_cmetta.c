/* Purpose: exercise every door of the C binding against a live engine, and
 *   fail loudly on the first one that does not behave as cmetta.h says.
 * Assumes: one runtime per process, so every case shares one engine and a
 *   case that writes to &self cleans up after itself.
 * Guarantees: exits 0 only when every case passed; prints the failing
 *   expression, its file and its line otherwise.
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#define MT_SHORTHAND
#include <cmetta.h>

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
              mt_errmsg() ? mt_errmsg() : "(none)");                   \
    }                                                                        \
  } while (0)

/* -DCMETTA_TRACE_CASES makes the harness announce each case, which is how a
   hang is located without a debugger. */
#ifdef MT_TRACE_CASES
#define CASE(name) \
  do { current_case = (name); fprintf(stderr, "CASE %s\n", (name)); } while (0)
#else
#define CASE(name) current_case = (name)
#endif

/* ================================================================== *
 * Atoms, which need no engine
 * ================================================================== */

static void test_atoms_need_no_engine(void)
{ mt_atom *sym, *text, *n, *f, *b, *v, *e, *unit;

  CASE("atoms are built and read without an engine");

  sym = S("foo");
  text = T("foo");
  CHECK(mt_kind_of(sym) == MT_SYMBOL);
  CHECK(mt_kind_of(text) == MT_TEXT);
  CHECK(strcmp(mt_name(sym), "foo") == 0);
  /* A symbol is not text; folding them together is the ambiguity the kinds
     exist to remove. */
  CHECK(!mt_eq(sym, text));

  n = N(42);
  f = R(2.0);
  b = B(true);
  CHECK(mt_kind_of(n) == MT_INT);
  CHECK(mt_kind_of(f) == MT_FLOAT);
  CHECK(mt_kind_of(b) == MT_BOOL);
  /* 2 and 2.0 are different atoms, which is why C splits the one wire tag. */
  { mt_atom *two = N(2);
    CHECK(!mt_eq(two, f));
    mt_drop(two);
  }
  /* A boolean is not a symbol that spells it. */
  { mt_atom *spelled = S("True");
    CHECK(!mt_eq(b, spelled));
    mt_drop(spelled);
  }

  v = V("x");
  CHECK(mt_kind_of(v) == MT_VARIABLE);

  e = E("+", 1, 2);
  CHECK(mt_kind_of(e) == MT_EXPR);
  CHECK(mt_len(e) == 3);
  CHECK(mt_kind_of(mt_at(e, 0)) == MT_SYMBOL);
  CHECK(mt_at(e, 3) == NULL);
  CHECK(mt_kind_of(mt_at(e, 3)) == MT_NONE);

  unit = mt_unit();
  CHECK(mt_kind_of(unit) == MT_EXPR);
  CHECK(mt_len(unit) == 0);
  /* Unit is not the empty string. */
  { mt_atom *empty = T("");
    CHECK(!mt_eq(unit, empty));
    mt_drop(empty);
  }

  mt_drop(sym); mt_drop(text); mt_drop(n); mt_drop(f);
  mt_drop(b); mt_drop(v); mt_drop(e); mt_drop(unit);
}

static void test_the_builder_coerces_each_child_by_its_c_type(void)
{ mt_atom *e, *nested;

  CASE("mt_expr coerces by C type and counts its own arguments");
  /* No count is written anywhere, and no child names a constructor. */
  e = E("edge", "a", 1, 2.5, V("y"));
  CHECK(mt_len(e) == 5);
  CHECK(mt_kind_of(mt_at(e, 0)) == MT_SYMBOL);   /* a bare string  */
  CHECK(mt_kind_of(mt_at(e, 1)) == MT_SYMBOL);   /* ...is a symbol */
  CHECK(mt_kind_of(mt_at(e, 2)) == MT_INT);
  CHECK(mt_kind_of(mt_at(e, 3)) == MT_FLOAT);
  CHECK(mt_kind_of(mt_at(e, 4)) == MT_VARIABLE);
  mt_drop(e);

  CASE("an atom argument passes through, so expressions nest");
  nested = E("f", E("g", 1), T("two"));
  CHECK(mt_len(nested) == 3);
  CHECK(mt_kind_of(mt_at(nested, 1)) == MT_EXPR);
  CHECK(mt_kind_of(mt_at(nested, 2)) == MT_TEXT);
  mt_drop(nested);

  CASE("every integer width reaches the same Number");
  { short s = 7; long l = 7; long long ll = 7; unsigned u = 7;
    mt_atom *a = E("f", s), *b = E("f", l), *c = E("f", ll), *d = E("f", u);
    CHECK(mt_eq(a, b) && mt_eq(b, c) && mt_eq(c, d));
    mt_drop(a); mt_drop(b); mt_drop(c); mt_drop(d);
  }
}

/* A macro that evaluates its argument twice is C's classic trap: mt_expr and
   the receiver dispatch both mention theirs more than once in their
   expansion, so "exactly once" is a property to test rather than assume. */
static int side_effects;
static metta *counted_runtime;
static int64_t bump(void)       { side_effects++; return 1; }
static const char *bump_s(void) { side_effects++; return "s"; }
static metta *bump_rt(void)     { side_effects++; return counted_runtime; }

static void test_a_macro_evaluates_each_argument_exactly_once(metta *m)
{ mt_atom *e;

  CASE("mt_expr evaluates each argument exactly once");
  side_effects = 0;
  e = E("f", bump(), bump_s(), bump());
  CHECK(e != NULL);
  CHECK(side_effects == 3);
  mt_drop(e);

  CASE("the _Generic receiver dispatch evaluates its target exactly once");
  /* MT_ON names the target twice: once as _Generic's controlling expression
     and once as the call's argument. The controlling expression is NOT
     evaluated -- only its type is read -- so the count must be one. */
  counted_runtime = m;
  side_effects = 0;
  (void)mt_count(bump_rt());
  CHECK(side_effects == 1);
}

static void test_a_failed_child_does_not_leak_its_siblings(void)
{ mt_atom *bad;
  CASE("a NULL child fails the whole expression");
  /* mt_spaceref refuses a name with no ampersand, so the middle child is
     NULL and the outer constructor must drop the two that succeeded rather
     than building something half-formed. Under a leak checker this is the
     case that proves the take-on-failure rule. */
  mt_clear();
  bad = E("f", mt_spaceref("nope"), 1);
  CHECK(bad == NULL);
  CHECK(!mt_ok());
}

static void test_refusals_are_named(void)
{ mt_atom *wide;

  CASE("a value C has no type for is refused by name");
  mt_clear();
  CHECK(mt_bigint("12x3") == NULL);
  CHECK(!mt_ok());
  CHECK(mt_rational(1, 0) == NULL);
  CHECK(mt_spaceref("kb") == NULL);

  mt_clear();
  wide = mt_bigint("170141183460469231731687303715884105728");
  CHECK(wide != NULL);
  CHECK(mt_kind_of(wide) == MT_BIGINT);
  CHECK(mt_ok());
  mt_drop(wide);
}

static void test_reading_promotes_only_where_it_is_lossless(void)
{ mt_atom *i = N(7), *f = R(2.5), *r = mt_rational(1, 4), *huge;

  CASE("an Int reads as a double, because nothing is lost");
  mt_clear();
  CHECK(mt_float(i) == 7.0);
  CHECK(mt_ok());

  CASE("a Float does NOT read as an Int, because rounding is not reading");
  mt_clear();
  CHECK(mt_int(f) == 0);
  CHECK(!mt_ok());

  CASE("a Rational reads as its quotient, and as its two halves");
  { mt_ratio parts = mt_ratio_of(r);
    CHECK(parts.num == 1 && parts.den == 4);
    /* A non-Rational answers a zero denominator, which no ratio has. */
    CHECK(mt_ratio_of(i).den == 0);
  }
  mt_clear();
  CHECK(mt_float(r) == 0.25);
  CHECK(mt_ok());

  CASE("an Int too wide for a double is refused rather than rounded");
  huge = N(9007199254740993LL);           /* 2^53 + 1 */
  mt_clear();
  CHECK(mt_float(huge) == 0.0);
  CHECK(!mt_ok());
  CHECK(mt_error() == MT_UNSUPPORTED);
  /* And it still reads exactly as what it is. */
  mt_clear();
  CHECK(mt_int(huge) == 9007199254740993LL);
  CHECK(mt_ok());

  mt_drop(i); mt_drop(f); mt_drop(r); mt_drop(huge);
}

static void test_the_error_state_is_errno_shaped(void)
{ CASE("a failure sticks until it is cleared, so a run is checked once");
  mt_clear();
  CHECK(mt_ok());
  CHECK(mt_errmsg() == NULL);
  CHECK(mt_error() == MT_OK);

  mt_drop(mt_bigint("nope"));
  CHECK(!mt_ok());
  CHECK(mt_errmsg() != NULL);

  /* A success afterwards does NOT clear it, which is the whole point: three
     reads can be checked with one test. */
  mt_drop(S("fine"));
  CHECK(!mt_ok());

  mt_clear();
  CHECK(mt_ok());
}

static void test_reference_counting_holds_under_churn(void)
{ int i;
  CASE("building, sharing and dropping atoms leaks nothing");
  for (i = 0; i < 2000; i++)
  { mt_atom *leaf = S("leaf");
    mt_atom *shared = mt_keep(leaf);
    mt_atom *outer = E("f", E(mt_keep(leaf), i), T("text"));
    const mt_atom *borrowed = mt_at(outer, 1);
    mt_atom *kept = mt_keep(borrowed);

    if ( i == 0 ) CHECK(mt_show(outer) != NULL);
    mt_drop(kept);
    mt_drop(outer);
    mt_drop(shared);
    mt_drop(leaf);
  }
}

/* ================================================================== *
 * Text, running, and the cursor
 * ================================================================== */

static void test_text_crosses_through_the_engine_reader(void)
{ mt_atom *parsed;

  CASE("parse and show use the engine's own reader and writer");
  parsed = mt_parse("(+ 1 2)");
  CHECK(parsed && mt_kind_of(parsed) == MT_EXPR);
  CHECK(mt_len(parsed) == 3);
  CHECK(strcmp(mt_show(parsed), "(+ 1 2)") == 0);
  mt_drop(parsed);

  CASE("show hands back storage it owns, so it drops into printf");
  { mt_atom *a = S("alpha"), *b = S("beta");
    /* Both renderings must still be readable in one call, which one slot
       could not manage. */
    const char *sa = mt_show(a), *sb = mt_show(b);
    CHECK(strcmp(sa, "alpha") == 0);
    CHECK(strcmp(sb, "beta") == 0);
    mt_drop(a); mt_drop(b);
  }

  CASE("a variable keeps the name its source gave it");
  parsed = mt_parse("(f $x $x)");
  /* Guarded, because a NULL parse must FAIL this case rather than crash it:
     the first version dereferenced straight through and turned a broken
     bridge into a segfault, which says nothing about what broke. */
  CHECK(parsed != NULL);
  if ( parsed )
  { CHECK(mt_kind_of(mt_at(parsed, 1)) == MT_VARIABLE);
    CHECK(strcmp(mt_name(mt_at(parsed, 1)), "x") == 0);
    CHECK(mt_eq(mt_at(parsed, 1), mt_at(parsed, 2)));
  }
  mt_drop(parsed);

  CASE("unreadable source is a refusal, not a wrong answer");
  mt_clear();
  CHECK(mt_parse("(unclosed") == NULL);
  CHECK(!mt_ok());
}

static void test_run_groups_answers_by_form(metta *m)
{ int seen = 0;
  size_t last_group = 0;

  CASE("run groups its answers by ! form, in source order");
  mt_rows (row, mt_run(m,
        "(= (twice $x) (* 2 $x))\n"
        "!(twice 21)\n"
        "!(superpose (a b))\n"))
  { const mt_atom *a = row->atom;
    last_group = row->group;
    if ( seen == 0 )
    { CHECK(mt_int(a) == 42);
      CHECK(last_group == 0);
    }
    if ( seen == 1 )
    { CHECK(mt_kind_of(a) == MT_SYMBOL);
      CHECK(last_group == 1);
    }
    CHECK(row->text != NULL);
    seen++;
  }
  CHECK(seen == 3);
  CHECK(last_group == 1);
}

static void test_the_walk_closes_its_cursor_on_break(metta *m)
{ int pulled = 0;

  CASE("eval computes one answer per step over an endless generator");
  /* Endless on purpose: an eager door cannot return from this at all, so the
     case passing IS the laziness proof, and `break` leaving the cursor closed
     is what makes it safe to write. */
  /* Run for its effect: a definition's point is what it leaves behind. */
  CHECK(mt_do(m, "(= (from $n) (superpose ($n (from (+ $n 1)))))"));

  mt_each (a, mt_eval(m, E("from", 0)))
  { CHECK(mt_int(a) == pulled);
    if ( ++pulled == 3 ) break;
  }
  CHECK(pulled == 3);

  CASE("two walks nest without their cursors colliding");
  { int pairs = 0;
    mt_each (x, mt_eval(m, E("superpose", E("a", "b"))))
    { mt_each (y, mt_eval(m, E("superpose", E("c", "d"))))
      { (void)x; (void)y; pairs++; }
    }
    CHECK(pairs == 4);
  }
}

static void test_one_and_first_make_different_claims(metta *m)
{ mt_atom *a;

  CASE("mt_one is a claim that there is exactly one answer");
  mt_clear();
  CHECK(mt_one_int(mt_eval(m, E("+", 1, 2))) == 3);
  CHECK(mt_ok());

  CASE("mt_one refuses a question that answered twice");
  mt_clear();
  a = mt_one(mt_eval(m, E("superpose", E("a", "b"))));
  CHECK(a == NULL);
  CHECK(mt_error() == MT_MISUSE);

  CASE("mt_first takes the first and makes no such claim");
  mt_clear();
  a = mt_first(mt_eval(m, E("superpose", E("a", "b"))));
  CHECK(a != NULL);
  CHECK(mt_ok());
  mt_drop(a);

  CASE("mt_one on no answers at all is a recorded failure");
  mt_clear();
  CHECK(mt_one(mt_eval(m, E("empty"))) == NULL);
  CHECK(!mt_ok());

  CASE("mt_all collects every answer as one owned array");
  { mt_list all = mt_all(mt_eval(m, E("superpose", E(1, 2, 3))));
    CHECK(all.len == 3);
    CHECK(all.items && mt_int(all.items[0]) + mt_int(all.items[1]) +
                       mt_int(all.items[2]) == 6);
    mt_list_free(all);
  }
}

/* ================================================================== *
 * Spaces
 * ================================================================== */

static void test_spaces_store_and_query(metta *m)
{ mt_space *kb;
  int matched = 0;

  CASE("a space stores, counts, matches and removes");
  kb = mt_space_open(m, "&cmetta-kb");
  CHECK(kb != NULL);
  CHECK(strcmp(mt_space_name(kb), "&cmetta-kb") == 0);

  CHECK(mt_add(kb, E("edge", "a", "b")));
  CHECK(mt_count(kb) == 1);

  mt_rows (r, mt_match(kb, E("edge", "a", V("y"))))
  { const mt_atom *got = r->atom;
    CHECK(mt_len(got) == 3);
    /* The pattern's variable arrives bound in the answer, reachable by the
       name the caller wrote rather than by counting children. */
    CHECK(strcmp(mt_name(mt_at(got, 2)), "b") == 0);
    CHECK(mt_bound(r, "y") != NULL);
    CHECK(strcmp(mt_name(mt_bound(r, "y")), "b") == 0);
    CHECK(mt_eq(mt_bound(r, "y"), mt_at(got, 2)));
    /* A name the pattern never had is NULL rather than a guess. */
    CHECK(mt_bound(r, "nosuch") == NULL);
    matched++;
  }
  CHECK(matched == 1);

  CASE("a name reaches a binding however deep the pattern puts it");
  CHECK(mt_add(kb, E("path", E("from", "a"), E("to", "z"))));
  mt_rows (row, mt_match(kb, E("path", E("from", V("s")),
                                        E("to", V("d")))))
  { CHECK(mt_bound(row, "s") && strcmp(mt_name(mt_bound(row, "s")), "a") == 0);
    CHECK(mt_bound(row, "d") && strcmp(mt_name(mt_bound(row, "d")), "z") == 0);
  }
  CHECK(mt_del(kb, E("path", E("from", "a"), E("to", "z"))));

  CASE("an eval cursor has no pattern, so it binds nothing rather than guessing");
  /* An eval answer is a reduced value, not an instance of the goal, so lining
     the two up would find a subterm at the same index and call it a binding.
     Asserted outside any loop, so the case holds whether or not the goal
     answered at all. */
  { mt_answers *cur = mt_eval(m, E("quote", V("x")));
    const mt_row *row;
    CHECK(cur != NULL);
    row = mt_row_next(cur);
    /* The row still carries its atom and text; only the binding is absent. */
    CHECK(row == NULL || row->atom != NULL);
    CHECK(row == NULL || mt_bound(row, "x") == NULL);
    mt_answers_free(cur);
  }

  CHECK(mt_del(kb, E("edge", "a", "b")) == true);
  CHECK(mt_count(kb) == 0);
  CHECK(mt_del(kb, E("edge", "a", "b")) == false);

  CHECK(mt_add(kb, E("edge", "a", "b")));
  CHECK(mt_wipe(kb));
  CHECK(mt_count(kb) == 0);
  mt_space_close(kb);
}

static void test_one_verb_takes_either_receiver(metta *m)
{ mt_space *kb;
  size_t before;

  CASE("the same verb points at a runtime or at a space");
  before = mt_count(m);                    /* a metta *  means &self   */
  CHECK(mt_add(m, E("cmetta-receiver-probe", 1)));
  CHECK(mt_count(m) == before + 1);

  kb = mt_space_open(m, "&cmetta-receiver");
  CHECK(mt_count(kb) == 0);                /* a mt_space * means it */
  CHECK(mt_add(kb, E("cmetta-receiver-probe", 1)));
  CHECK(mt_count(kb) == 1);
  /* The two receivers are different stores, which is the point. */
  CHECK(mt_count(m) == before + 1);

  CHECK(mt_del(m, E("cmetta-receiver-probe", 1)));
  CHECK(mt_wipe(kb));
  mt_space_close(kb);
}

static void test_a_user_space_decodes_as_a_space(metta *m)
{ CASE("a space the engine made decodes as MT_SPACE, not a symbol");
  mt_each (a, mt_run(m, "!(new-space)"))
  { CHECK(mt_kind_of(a) == MT_SPACE);
    CHECK(mt_name(a)[0] == '&');
  }

  CASE("an ampersand name that is no space stays a symbol");
  mt_each (a, mt_run(m, "!(id &not-a-space)"))
  { const mt_atom *arg = mt_kind_of(a) == MT_EXPR
                          ? mt_at(a, mt_len(a) - 1) : a;
    CHECK(mt_kind_of(arg) == MT_SYMBOL);
  }
}

/* ================================================================== *
 * Published C functions
 * ================================================================== */

static mt_status op_double(mt_call *call, void *user)
{ int64_t v;
  (void)user;
  if ( mt_arity(call) != 1 ) return MT_FAIL;
  mt_clear();
  v = mt_int(mt_arg(call, 0));
  if ( !mt_ok() ) return mt_fail(call, "double wants a Number");
  return mt_answer(call, N(v * 2));
}

static mt_status op_tag_it(mt_call *call, void *user)
{ return mt_answer(call, E((const char *)user,
                              mt_keep(mt_arg(call, 0))));
}

static void test_a_c_function_is_callable_from_metta(metta *m)
{ CASE("a published C function answers a MeTTa call");
  CHECK(mt_def(m, (mt_op){ .name = "cdouble", .arity = 1,
                                 .effect = MT_PURE, .fn = op_double }));
  CHECK(mt_one_int(mt_run(m, "!(cdouble 21)")) == 42);

  CASE("a C name spelled with underscores reaches MeTTa with hyphens");
  CHECK(mt_def(m, (mt_op){ .name = "tag_it", .arity = 1,
                                 .effect = MT_PURE, .fn = op_tag_it,
                                 .user = (void *)"tagged" }));
  { mt_atom *got = mt_one(mt_run(m, "!(tag-it 7)"));
    CHECK(got && mt_kind_of(got) == MT_EXPR);
    CHECK(mt_len(got) == 2);
    CHECK(strcmp(mt_name(mt_at(got, 0)), "tagged") == 0);
    mt_drop(got);
  }

  CASE("a C function's refusal reaches the caller as an error");
  mt_clear();
  mt_answers_free(mt_run(m, "!(cdouble \"not a number\")"));
  CHECK(!mt_ok());
  /* The WORDS, not just the status. The error term's functor is a contract
     with bridge.pl's prolog:error_message//1, and when the two drifted apart
     the status was still right while the text read "Unknown error term:
     ...", which every check that only asked mt_ok() sailed past. */
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "double wants a Number") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "Unknown error term") == NULL);

  CASE("an operation must name one of the five effect classes");
  mt_clear();
  CHECK(!mt_def(m, (mt_op){ .name = "bogus", .arity = 1,
                                  .effect = (mt_effect)99, .fn = op_double }));
  CHECK(mt_error() == MT_MISUSE);

  CASE("a withdrawn name is data again");
  CHECK(mt_undef(m, "cdouble"));
  mt_each (a, mt_run(m, "!(cdouble 21)"))
      CHECK(mt_kind_of(a) == MT_EXPR);
  CHECK(mt_undef(m, "tag_it"));
}

typedef struct { int bumps; } counter;

static mt_status op_bump(mt_call *call, void *user)
{ const mt_atom *handle = mt_arg(call, 0);
  counter *c;
  (void)user;
  if ( mt_kind_of(handle) != MT_OBJECT )
    return mt_fail(call, "bump wants the counter it was given");
  CHECK(strcmp(mt_type(handle), "counter") == 0);
  c = mt_value(handle);
  c->bumps++;
  return mt_answer(call, N(c->bumps));
}

/* One body, two languages: the operators are parameters, so this expands to C
   in one mode and to MeTTa tokens in the other. */
#define POLY_BODY(ADD, MUL, x)  ADD(MUL(3, x), 1)
#define POLY_C_ADD(a, b)        ((a) + (b))
#define POLY_C_MUL(a, b)        ((a) * (b))
#define POLY_M_ADD(a, b)        (+ a b)
#define POLY_M_MUL(a, b)        (* a b)

static int64_t poly_in_c(int64_t x) { return POLY_BODY(POLY_C_ADD, POLY_C_MUL, x); }

static void test_a_c_body_lowers_into_an_equation_the_engine_can_see(metta *m)
{ CASE("mt_lower installs an equation from C tokens");
  CHECK(mt_lower(m, (lowered-twice $x), (* 2 $x)));
  CHECK(mt_one_int(mt_run(m, "!(lowered-twice 21)")) == 42);

  CASE("a nested body lowers whole");
  CHECK(mt_lower(m, (fib $n), (if (< $n 2) $n
                                  (+ (fib (- $n 1)) (fib (- $n 2))))));
  CHECK(mt_one_int(mt_run(m, "!(fib 10)")) == 55);

  CASE("one body reaches C and MeTTa, and the two agree");
  CHECK(mt_lower(m, (poly $x), POLY_BODY(POLY_M_ADD, POLY_M_MUL, $x)));
  CHECK(mt_one_int(mt_run(m, "!(poly 5)")) == 16);
  CHECK(poly_in_c(5) == 16);
  CHECK(mt_one_int(mt_run(m, "!(poly 7)")) == poly_in_c(7));

  CASE("what lowering buys over mt_def: the engine can SEE the equation");
  /* A published C function is opaque, so nothing can be asked about it. An
     equation is MeTTa, so it is in the space and matches like any other atom.
     That is the whole difference, and it is why lowering is worth having. */
  { int found = 0;
    mt_each (a, mt_match(mt_self(m), E("=", E("poly", V("x")), V("body"))))
    { CHECK(mt_kind_of(a) == MT_EXPR);
      found++;
    }
    CHECK(found == 1);
  }

  CASE("a lowered name is a function, so it composes with the rest");
  CHECK(mt_one_int(mt_run(m, "!(lowered-twice (poly 5))")) == 32);
}

static void test_a_c_value_crosses_by_reference(metta *m)
{ static counter c = {0};
  mt_atom *handle;

  CASE("a live C value crosses MeTTa and comes back the same object");
  CHECK(mt_def(m, (mt_op){ .name = "bump", .arity = 1,
                                 .effect = MT_WRITES, .fn = op_bump }));
  handle = mt_object(&c, "counter", NULL);
  CHECK(handle != NULL);
  CHECK(mt_kind_of(handle) == MT_OBJECT);
  CHECK(mt_value(handle) == &c);

  /* State behind the handle survives across MeTTa calls. */
  CHECK(mt_one_int(mt_eval(m, E("bump", mt_keep(handle)))) == 1);
  CHECK(mt_one_int(mt_eval(m, E("bump", mt_keep(handle)))) == 2);
  CHECK(c.bumps == 2);

  mt_drop(handle);
  CHECK(mt_undef(m, "bump"));
}

static mt_status fn_triple(mt_call *call, void *user)
{ int64_t v;
  (void)user;
  mt_clear();
  v = mt_int(mt_arg(call, 0));
  if ( !mt_ok() ) return MT_FAIL;
  return mt_answer(call, N(v * 3));
}

static void test_a_function_value_is_applicable(metta *m)
{ mt_atom *fn;

  CASE("a C function carried as a value is applied where it lands");
  fn = mt_function(fn_triple, NULL, NULL);
  CHECK(fn != NULL);
  CHECK(mt_one_int(mt_eval(m, E(mt_keep(fn), 5))) == 15);
  mt_drop(fn);
}

/* ================================================================== *
 * Errors, wide values, bounds and counters
 * ================================================================== */

static void test_an_engine_error_reaches_c_as_words(metta *m)
{ /* A raise, not a value. MeTTa keeps most failures AS values -- (car-atom 5)
     answers unit and (+ 1 foo) answers itself unreduced -- so the case needs
     something that genuinely throws, and a failed assertion does. */
  CASE("an engine exception crosses as MT_ERROR and readable words");
  mt_clear();
  CHECK(mt_run(m, "!(assertEqual 1 2)") == NULL);
  CHECK(mt_error() == MT_ERROR);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "ssertion") != NULL);

  CASE("the runtime is still usable after one call raised");
  mt_clear();
  CHECK(mt_one_int(mt_run(m, "!(+ 1 2)")) == 3);
  CHECK(mt_ok());

  CASE("an error kept as a VALUE stays an ordinary answer");
  { mt_atom *got = mt_one(mt_run(m, "!(Error foo bar)"));
    CHECK(got && mt_kind_of(got) == MT_EXPR);
    CHECK(strcmp(mt_name(mt_at(got, 0)), "Error") == 0);
    mt_drop(got);
  }
}

static void test_a_wide_integer_keeps_its_digits(metta *m)
{ mt_atom *got;

  CASE("an integer past int64 arrives as BIGINT with its exact digits");
  got = mt_one(mt_run(m, "!(* 9223372036854775807 4)"));
  CHECK(got && mt_kind_of(got) == MT_BIGINT);
  CHECK(strcmp(mt_name(got), "36893488147419103228") == 0);
  /* And it refuses to pretend it fits. */
  mt_clear();
  CHECK(mt_int(got) == 0);
  CHECK(!mt_ok());
  mt_drop(got);
}

static void test_variable_identity_survives_the_round_trip(void)
{ mt_atom *same, *different;
  char *a, *b;

  CASE("two occurrences of one name are one variable, two names are two");
  same = E("f", V("x"), V("x"));
  different = E("f", V("x"), V("y"));
  a = mt_show_dup(same);
  b = mt_show_dup(different);
  CHECK(a && b && strcmp(a, b) != 0);
  CHECK(a && strcmp(a, "(f $x $x)") == 0);
  mt_free(a);
  mt_free(b);
  mt_drop(same);
  mt_drop(different);
}

static void test_a_bound_stops_a_runaway_and_says_so(metta *m)
{ mt_limits bounded = {0}, none = {0};
  int pulled = 0;

  CASE("an inference bound stops an endless evaluation as MT_LIMIT");
  /* Sized from the measurement, not guessed: an answer of (from $n) costs
     roughly 40 engine inferences, so 20,000 buys a few hundred answers and
     then stops. The budget is CUMULATIVE across steps. */
  bounded.inferences = 20000;
  CHECK(mt_limit(m, bounded));

  mt_clear();
  /* The ceiling is a BACKSTOP: the bound should stop this long before
     200,000 answers, and it is here so a broken bound FAILS the case in
     seconds instead of hanging the gate. */
  mt_each (a, mt_eval(m, E("from", 0)))
  { (void)a;
    if ( ++pulled >= 200000 ) break;
  }
  CHECK(pulled > 0);
  CHECK(pulled < 200000);
  CHECK(mt_error() == MT_LIMIT);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "inference") != NULL);

  CASE("the same bound applied to a whole run");
  mt_clear();
  CHECK(mt_run(m, "!(from 0)") == NULL);
  CHECK(mt_error() == MT_LIMIT);

  CASE("clearing the bounds restores unbounded evaluation");
  CHECK(mt_limit(m, none));
  CHECK(mt_limits_of(m).inferences == 0);
  mt_clear();
  CHECK(mt_one_int(mt_run(m, "!(+ 1 2)")) == 3);
  CHECK(mt_ok());
}

static void test_the_counters_measure_engine_work(metta *m)
{ mt_stats before, after, spent;

  CASE("the engine's counters move with the work, and the same way twice");
  before = mt_stats_now(m);
  mt_each (x, mt_run(m, "!(superpose (1 2 3 4 5))")) (void)x;
  after = mt_stats_now(m);
  spent = mt_stats_since(before, after);
  CHECK(spent.inferences > 0);
  CHECK(after.cputime >= before.cputime);

  /* Inferences are deterministic where wall clock is not, which is the whole
     reason this tree gates on them: the same workload twice costs the same. */
  { mt_stats b1, a1, b2, a2;
    b1 = mt_stats_now(m);
    mt_each (x, mt_run(m, "!(superpose (1 2 3 4 5))")) (void)x;
    a1 = mt_stats_now(m);

    b2 = mt_stats_now(m);
    mt_each (x, mt_run(m, "!(superpose (1 2 3 4 5))")) (void)x;
    a2 = mt_stats_now(m);

    CHECK(mt_stats_since(b1, a1).inferences ==
          mt_stats_since(b2, a2).inferences);
  }
}

static void test_verbosity_reaches_the_engines_own_door(metta *m)
{ bool was;

  /* mt_verbose() records the new setting only when the Prolog call came
     back MT_OK, so a round trip that reports back what was just set is
     proof the call ran. It is worth its own case because the predicate it
     reaches moved: bridge.pl used to define metta_c_set_silent/1 in `user`,
     and this now calls engine/filereader.pl's metta_host_set_silent/1, which
     reaches `user` by EXPORT rather than by being defined there.

     Whether the engine then keeps its diagnostics off this process's stdout
     is asserted where a test can see a file descriptor after the process has
     flushed it: the Python seat's C-binding test reads THIS binary's
     streams. */
  CASE("mt_verbose reaches the engine's published verbosity door");
  was = mt_verbose(m, true);
  CHECK(mt_verbose(m, false) == true);
  CHECK(mt_verbose(m, was) == false);
}

static void test_reopening_is_the_same_runtime(metta *m)
{ CASE("a second open hands back the one runtime this process has");
  mt_clear();
  CHECK(mt_open(NULL) == m);
  CHECK(mt_ok());
  { mt_config other = { .path = "/definitely/not/here" };
    CHECK(mt_open(&other) == NULL);
    CHECK(mt_error() == MT_MISUSE);
    CHECK(mt_errmsg() != NULL);
  }
}

#ifdef MT_HAS_AUTO
static void test_scope_cleanup_releases_on_every_exit(metta *m)
{ CASE("MT_AUTO releases whatever way the block is left");
  { MT_AUTO mt_atom *held = mt_one(mt_eval(m, E("+", 1, 1)));
    CHECK(mt_int(held) == 2);
  } /* dropped here */

  { MT_AUTO_ASK mt_answers *r = mt_run(m, "!(superpose (1 2 3))");
    CHECK(mt_next(r) != NULL);
  } /* closed here, with two answers still uncomputed */

  /* And a value can be handed out of such a block without being released. */
  { mt_atom *escaped;
    { MT_AUTO mt_atom *tmp = S("kept");
      escaped = MT_TAKE(tmp);
    }
    CHECK(escaped && strcmp(mt_name(escaped), "kept") == 0);
    mt_drop(escaped);
  }
}
#endif

int main(void)
{ metta *m = mt_open(NULL);

  if ( !m )
  { fprintf(stderr, "cannot boot the engine: %s\n", mt_errmsg());
    return 1;
  }

  test_atoms_need_no_engine();
  test_the_builder_coerces_each_child_by_its_c_type();
  test_a_macro_evaluates_each_argument_exactly_once(m);
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
  test_a_c_body_lowers_into_an_equation_the_engine_can_see(m);
  test_a_c_value_crosses_by_reference(m);
  test_a_function_value_is_applicable(m);
  test_an_engine_error_reaches_c_as_words(m);
  test_a_wide_integer_keeps_its_digits(m);
  test_variable_identity_survives_the_round_trip();
  test_a_bound_stops_a_runaway_and_says_so(m);
  test_the_counters_measure_engine_work(m);
  test_verbosity_reaches_the_engines_own_door(m);
  test_reopening_is_the_same_runtime(m);
#ifdef MT_HAS_AUTO
  test_scope_cleanup_releases_on_every_exit(m);
#endif

  printf("%d checks, %d failures\n", checks, failures);
  mt_close(m);
  return failures == 0 ? 0 : 1;
}
