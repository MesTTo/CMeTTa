/* Purpose: exercise every door of the C binding against a live engine, and
 *   fail loudly on the first one that does not behave as cmetta.h says.
 * Assumes: one runtime per process, so every case shares one engine and a
 *   case that writes to &self cleans up after itself.
 * Guarantees: exits 0 only when every case passed; prints the failing
 *   expression, its file and its line otherwise.
 *   Engine-owned &self and &metta refuse wipe without damaging catalog,
 *   typing, or arithmetic state; an ordinary named space still wipes
 *   [tested: test_engine_owned_base_spaces_refuse_wipe; commit=6229e43cb68cc3685360810d462d992874992f6c].
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#define MT_SHORTHAND
#include <cmetta.h>
#include <SWI-Prolog.h>

#include <math.h>
#include <stdio.h>
#include <stdint.h>
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

static void test_public_scalar_readers_cover_their_whole_domain(void)
{ static const char counted[] = { 'a', '\0', 'b' };
  static const char *statuses[] = {
    "ok", "row", "done", "no answer", "engine error", "out of memory",
    "misuse", "unsupported value", "stopped by a bound"
  };
  static const char *effects[] = {
    "pureStructural", "readOnlyLookup", "nondeterministicReadOnly",
    "writesState", "oracleIO"
  };
  mt_atom *text = mt_textn(counted, sizeof(counted));
  mt_atom *yes = B(true), *no = B(false), *wrong = S("true");
  size_t i;

  CASE("counted names and truth values are read without losing their domain");
  CHECK(text && mt_name_len(text) == sizeof(counted));
  CHECK(yes && mt_truth(yes));
  CHECK(no && !mt_truth(no));
  CHECK(mt_ok());
  mt_clear();
  CHECK(!mt_truth(wrong));
  CHECK(mt_error() == MT_MISUSE);
  mt_clear();

  CASE("every public status and effect has one stable name");
  for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++)
    CHECK(strcmp(mt_status_str((mt_status)i), statuses[i]) == 0);
  CHECK(strcmp(mt_status_str((mt_status)99), "unknown status") == 0);
  for (i = 0; i < sizeof(effects) / sizeof(effects[0]); i++)
    CHECK(strcmp(mt_effect_str((mt_effect)i), effects[i]) == 0);
  CHECK(mt_effect_str((mt_effect)99) == NULL);
  CHECK(strcmp(mt_version(), "0.1.0") == 0);

  mt_drop(text);
  mt_drop(yes);
  mt_drop(no);
  mt_drop(wrong);
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

/* Called BEFORE mt_open(), which is the only moment this can be asked. Every
   door that reaches the engine used to die inside PL_open_foreign_frame with
   no thread environment to read: sixteen of them, from mt_parse to
   mt_stats_now, each a SIGSEGV a host cannot catch [measured 2026-08-31,
   ai-tmp/cseat-probe-d3.c; C34 in ai-cmetta-c-constraints.md]. The case
   failing takes the whole binary down with it, which is exactly what the
   defect did to a caller. */
static void test_a_door_before_the_runtime_refuses(void)
{ mt_atom *x = S("x");

  CASE("a door called before mt_open refuses by name rather than dying");

  mt_clear();
  CHECK(mt_parse("(f 1)") == NULL);
  CHECK(mt_error() == MT_MISUSE);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "mt_open") != NULL);

  mt_clear();
  CHECK(strcmp(mt_show(x), "<unwritable>") == 0);
  CHECK(mt_error() == MT_MISUSE);

  /* The runtime a failed mt_open() hands back is NULL, and every door that
     takes one has to survive being given it. */
  mt_clear();
  CHECK(mt_run(NULL, "!(+ 1 2)") == NULL);
  CHECK(mt_do(NULL, "(= (f) 1)") == false);
  CHECK(mt_load(NULL, "/nonexistent.metta") == NULL);
  CHECK(mt_self(NULL) == NULL);
  CHECK(mt_catalog(NULL) == NULL);
  CHECK(mt_space_open(NULL, "&kb") == NULL);
  CHECK(mt_stats_now(NULL).inferences == 0);
  CHECK(mt_limits_of(NULL).seconds == 0.0);
  CHECK(mt_undef(NULL, "nothing") == false);
  CHECK(mt_thread_attach() == false);
  CHECK(mt_error() == MT_MISUSE);

  /* A door that TAKES an atom still takes it: the refusal is not a leak. */
  mt_clear();
  CHECK(mt_self_add(NULL, S("dropped-anyway")) == false);
  CHECK(mt_self_del(NULL, S("dropped-anyway")) == false);
  CHECK(mt_self_count(NULL) == 0);
  CHECK(mt_self_eval(NULL, E("+", 1, 2)) == NULL);
  CHECK(mt_self_match(NULL, V("x")) == NULL);
  CHECK(mt_self_atoms(NULL) == NULL);

  /* A release door is a no-op rather than a refusal: tidying up must not
     depend on the order it is done in. */
  mt_answers_free(NULL);
  mt_space_close(NULL);
  mt_close(NULL);
  mt_thread_detach();

  mt_drop(x);
  mt_clear();
}

typedef struct { int calls; } release_probe;

static void count_release(void *value)
{ release_probe *probe = value;
  probe->calls++;
}

static void test_an_uncrossed_object_can_be_released_without_an_engine(void)
{ release_probe probe = {0};

  CASE("mt_object_free consumes an uncrossed object before the engine exists");
  CHECK(mt_object_free(mt_object(&probe, "release-probe", count_release)));
  CHECK(probe.calls == 1);
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

/* Rule 2 of cmetta.h: every function that CAN FAIL says so. Three
   constructors answered NULL through a ternary that short-circuited before
   the failure could be recorded, so `mt_clear(); mt_sym(NULL);` left the
   thread reporting ok [measured 2026-08-31; C33 in
   ai-cmetta-c-constraints.md]. */
static void test_a_failed_constructor_says_so(void)
{ CASE("a constructor that answers NULL leaves the reason behind it");

  mt_clear();
  CHECK(mt_sym(NULL) == NULL);
  CHECK(mt_error() == MT_MISUSE);
  CHECK(mt_errmsg() != NULL);

  mt_clear();
  CHECK(mt_var(NULL) == NULL);
  CHECK(mt_error() == MT_MISUSE);

  mt_clear();
  CHECK(mt_text(NULL) == NULL);
  CHECK(mt_error() == MT_MISUSE);

  mt_clear();
  CHECK(mt_textn(NULL, 0) == NULL);
  CHECK(mt_error() == MT_MISUSE);

  /* A count with no array is the same shape one level up, and it used to be
     a read through NULL rather than a refusal. */
  mt_clear();
  CHECK(mt_exprv(3, NULL) == NULL);
  CHECK(mt_error() == MT_MISUSE);

  /* Unit is still built from no children at all. */
  mt_clear();
  { mt_atom *unit = mt_exprv(0, NULL);
    CHECK(unit != NULL);
    CHECK(mt_len(unit) == 0);
    CHECK(mt_ok());
    mt_drop(unit);
  }
}

/* A ratio is stored the way the engine keeps one, because a form the engine's
   reader refuses is a term that cannot cross: SWI writes -1r2 and refuses
   1r-2, so a negative denominator built an atom mt_show() could not render
   [measured 2026-08-31; C32 in ai-cmetta-c-constraints.md]. */
static void test_a_ratio_is_stored_in_canonical_form(void)
{ mt_atom *a;

  CASE("the sign sits on the numerator and the pair is in lowest terms");
  mt_clear();
  a = mt_rational(1, -2);
  CHECK(a != NULL);
  CHECK(mt_ratio_of(a).num == -1 && mt_ratio_of(a).den == 2);
  CHECK(mt_float(a) == -0.5);
  /* And it crosses: this is the door that refused before. */
  CHECK(strcmp(mt_show(a), "-1r2") == 0);
  CHECK(mt_ok());
  mt_drop(a);

  a = mt_rational(2, 4);
  CHECK(mt_ratio_of(a).num == 1 && mt_ratio_of(a).den == 2);
  mt_drop(a);

  a = mt_rational(-6, -8);
  CHECK(mt_ratio_of(a).num == 3 && mt_ratio_of(a).den == 4);
  mt_drop(a);

  a = mt_rational(0, -5);
  CHECK(mt_ratio_of(a).num == 0 && mt_ratio_of(a).den == 1);
  mt_drop(a);

  CASE("a ratio whose canonical form does not fit is refused by name");
  mt_clear();
  /* INT64_MIN is the one denominator whose sign cannot move to the
     numerator, and 3 shares no factor with it. */
  CHECK(mt_rational(3, INT64_MIN) == NULL);
  CHECK(mt_error() == MT_UNSUPPORTED);
}

/* The other half of the same canonicalisation, and the half the ENGINE
   decides: SWI evaluates `3 rdiv 1` to 3, so a whole-number ratio came back
   from a space as an Int and mt_eq() answered false against the atom that had
   been stored. Reading it back as a ratio still answers 3/1, because that
   promotion is exact and rule 5 says to take it. */
static void test_a_ratio_is_canonical_in_both_halves(metta *m)
{ mt_atom *whole;
  const mt_atom *stored;

  CASE("a canonical denominator of one is an Int, and reads as n over 1");
  mt_clear();
  whole = mt_rational(3, 1);
  CHECK(whole != NULL);
  CHECK(mt_kind_of(whole) == MT_INT);
  CHECK(mt_int(whole) == 3);
  CHECK(mt_ratio_of(whole).num == 3 && mt_ratio_of(whole).den == 1);
  { mt_atom *three = N(3);
    CHECK(three != NULL);
    CHECK(mt_eq(whole, three));
    mt_drop(three);
  }
  CHECK(mt_ok());

  CASE("and it comes back from a space as the atom that went in");
  CHECK(mt_add(m, mt_expr("ratio-round-trip", mt_keep(whole))));
  mt_rows (row, mt_match(m, mt_expr("ratio-round-trip", mt_var("x"))))
  { stored = mt_bound(row, "x");
    CHECK(stored != NULL);
    CHECK(mt_kind_of(stored) == MT_INT);
    CHECK(mt_eq(stored, whole));
  }
  CHECK(mt_del(m, mt_expr("ratio-round-trip", mt_keep(whole))));
  mt_drop(whole);
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
  mt_clear();                       /* the Float refusal above is still set */
  { mt_ratio parts = mt_ratio_of(r);
    CHECK(parts.num == 1 && parts.den == 4);
    /* An Int reads as itself over one: the promotion is exact, so rule 5 takes
       it. This line asserted a refusal until 2026-08-31, and the refusal was
       unreachable-in-principle once mt_rational canonicalised a denominator of
       one to an Int -- mt_ratio_of would then have refused the very atom
       mt_rational built. The ENGINE settles it: SWI evaluates `3 rdiv 1` to 3,
       so a whole-number ratio comes back from a space as an Int, and a seat
       that called that "not a ratio" would disagree with the engine it drives.
       A Bigint still refuses, which is where the lossless path really ends. */
    CHECK(mt_ratio_of(i).num == 7 && mt_ratio_of(i).den == 1);
    CHECK(mt_ok());
    mt_clear();
    CHECK(mt_ratio_of(f).den == 0);
    CHECK(!mt_ok());
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

static void test_presentation_and_round_trip_text_are_distinct(void)
{ static const char counted[] = { 'a', 'b', '\0', 'c', 'd' };
  mt_atom *text = mt_textn(counted, sizeof(counted));
  mt_atom *read_back;
  char *shown;
  mt_string written;

  CASE("mt_show is presentation, while mt_write_dup round trips counted text");
  shown = mt_show_dup(text);
  CHECK(shown != NULL);
  CHECK(shown && strlen(shown) == 3);       /* quote, a, b, then the NUL */
  mt_free(shown);

  written = mt_write_dup(text);
  CHECK(written.data != NULL);
  CHECK(written.len == 7);                 /* quotes plus all five bytes */
  CHECK(written.data && written.data[0] == '"');
  CHECK(written.data && written.data[3] == '\0');
  CHECK(written.data && written.data[written.len - 1] == '"');
  read_back = written.data ? mt_parsen(written.data, written.len) : NULL;
  CHECK(read_back != NULL);
  CHECK(mt_eq(text, read_back));
  mt_drop(read_back);
  mt_free(written.data);
  mt_drop(text);

  CASE("strict writing refuses a presentation spelling that would read wrong");
  { mt_atom *spaced = S("has space");
    mt_clear();
    written = mt_write_dup(spaced);
    CHECK(written.data == NULL && written.len == 0);
    CHECK(mt_error() == MT_ERROR);
    CHECK(mt_errmsg() && strstr(mt_errmsg(), "printed form would read back"));
    CHECK(strcmp(mt_show(spaced), "has space") == 0);
    mt_drop(spaced);
  }

  CASE("non-finite floats display but have no round-trip source spelling");
  { mt_atom *infinite = R(INFINITY);
    mt_clear();
    CHECK(strcmp(mt_show(infinite), "inf") == 0);
    written = mt_write_dup(infinite);
    CHECK(written.data == NULL && written.len == 0);
    CHECK(mt_error() == MT_ERROR);
    CHECK(mt_errmsg() && strstr(mt_errmsg(), "printed form would read back"));
    mt_drop(infinite);
  }
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

  CASE("the scalar one-answer readers release their answer behind them");
  mt_clear();
  CHECK(mt_one_truth(mt_eval(m, B(true))));
  CHECK(mt_ok());
  CHECK(strcmp(mt_one_name(mt_eval(m, S("one-name"))), "one-name") == 0);
  CHECK(mt_ok());
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

static void test_catalog_and_file_load_are_live_runtime_doors(metta *m)
{ mt_space *catalog = mt_catalog(m);
  mt_answers *loaded;
  const char *fixture = MT_ENGINE_PATH
                        "/extensions/cmetta/tests/fixtures/load_test.metta";

  CASE("the catalog handle names and queries the live &metta space");
  CHECK(catalog != NULL);
  CHECK(catalog && strcmp(mt_space_name(catalog), "&metta") == 0);
  CHECK(catalog && mt_count(catalog) > 0);

  CASE("mt_load reaches the reload-aware real-file door");
  loaded = mt_load(m, fixture);
  CHECK(loaded != NULL);
  mt_answers_free(loaded);
  CHECK(mt_one_int(mt_run(m, "!(cmetta-loaded-value)")) == 73);
  CHECK(mt_ok());

  loaded = mt_load(m, fixture);
  CHECK(loaded != NULL);
  mt_answers_free(loaded);
  CHECK(mt_one_int(mt_run(m, "!(cmetta-loaded-value)")) == 73);
  CHECK(mt_ok());
}

static void test_engine_owned_base_spaces_refuse_wipe(metta *m)
{ mt_space *catalog = mt_catalog(m);
  mt_space *ordinary = mt_space_open(m, "&cmetta-base-clear-control");
  const char *type_name;
  size_t catalog_before = mt_count(catalog);

  CASE("&self refuses wipe with the caller-owned-space remedy");
  mt_clear();
  CHECK(!mt_self_wipe(m));
  CHECK(mt_error() == MT_ERROR);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "&self") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "clear") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "caller's own context space") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "named space") != NULL);

  CASE("&metta refuses wipe with the caller-owned-space remedy");
  mt_clear();
  CHECK(!mt_space_wipe(catalog));
  CHECK(mt_error() == MT_ERROR);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "&metta") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "clear") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "caller's own context space") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "named space") != NULL);

  CASE("the refusals preserve catalog, typing and arithmetic");
  mt_clear();
  CHECK(mt_count(catalog) == catalog_before);
  type_name = mt_one_name(mt_run(m, "!(get-type 1)"));
  CHECK(type_name && strcmp(type_name, "Number") == 0);
  CHECK(mt_one_int(mt_run(m, "!(+ 1 2)")) == 3);
  CHECK(mt_ok());

  CASE("an ordinary named space still wipes");
  CHECK(ordinary != NULL);
  CHECK(mt_add(ordinary, E("ordinary", "clear")));
  CHECK(mt_count(ordinary) == 1);
  CHECK(mt_space_wipe(ordinary));
  CHECK(mt_count(ordinary) == 0);
  mt_space_close(ordinary);
}

/* A NULL atom is what a failed constructor hands a door, and the errno shape
   invites checking later rather than at once, so the doors have to survive
   it. They did not: space_call() wrote nothing into av[1], the bridge read
   the unbound variable as a WILDCARD, and mt_del(space, NULL) removed every
   atom in the space while mt_add(space, NULL) stored a fresh variable -- both
   answering true with the error state clean [measured 2026-08-31,
   ai-tmp/cseat-probe-d1.c; C32 in ai-cmetta-c-constraints.md]. */
static void test_a_door_that_takes_an_atom_refuses_null(metta *m)
{ mt_space *kb = mt_space_open(m, "&cmetta-null-atom");
  release_probe probe = {0};

  CASE("operation callback doors refuse a NULL call instead of dereferencing it");
  mt_clear();
  CHECK(mt_arity(NULL) == 0);
  CHECK(mt_error() == MT_MISUSE);
  mt_clear();
  CHECK(mt_arg(NULL, 0) == NULL);
  CHECK(mt_error() == MT_MISUSE);
  mt_clear();
  CHECK(mt_of(NULL) == NULL);
  CHECK(mt_error() == MT_MISUSE);
  mt_clear();
  CHECK(mt_answer(NULL,
                  mt_object(&probe, "null-call-release", count_release)) ==
        MT_MISUSE);
  CHECK(probe.calls == 1);
  CHECK(mt_error() == MT_MISUSE);
  mt_clear();
  CHECK(mt_fail(NULL, "unused") == MT_MISUSE);
  CHECK(mt_error() == MT_MISUSE);

  CASE("a write door refuses a NULL atom instead of matching everything");
  CHECK(kb != NULL);
  CHECK(mt_add(kb, E("keep", "me")));
  CHECK(mt_add(kb, E("keep", "me-too")));
  CHECK(mt_count(kb) == 2);

  mt_clear();
  CHECK(mt_del(kb, NULL) == false);
  CHECK(mt_error() == MT_MISUSE);
  CHECK(mt_count(kb) == 2);

  mt_clear();
  CHECK(mt_add(kb, NULL) == false);
  CHECK(mt_error() == MT_MISUSE);
  CHECK(mt_count(kb) == 2);

  /* The same door, one level up: a constructor that failed inside the call. */
  mt_clear();
  CHECK(mt_del(kb, E("keep", mt_spaceref("no-ampersand"))) == false);
  CHECK(!mt_ok());
  CHECK(mt_count(kb) == 2);

  mt_clear();
  CHECK(mt_space_eval(kb, NULL) == NULL);
  CHECK(mt_error() == MT_MISUSE);
  CHECK(mt_space_match(kb, NULL) == NULL);
  CHECK(mt_error() == MT_MISUSE);

  CHECK(mt_wipe(kb));
  mt_space_close(kb);
}

/* Every walk over a term used to recurse once per level of nesting, and all
   five died on data, each a SIGSEGV rather than a refusal: decode, encode and
   mt_bound at 80,000 levels, mt_eq at 200,000, mt_drop at 400,000 [measured
   2026-08-31 on this box's 8 MB thread stack, ai-tmp/cseat-probe-d4.c; C35 in
   ai-cmetta-c-constraints.md].

   Two depths because the walks cost differently, and each is past the crash
   point of the walks it drives. The three that CROSS to the engine make it
   copy the term as well, which is where the memory goes: this case measures
   0.5s and 208 MB at 100,000, and 1.1s and 427 MB when the crossing runs at
   400,000 too [measured 2026-08-31, /usr/bin/time on tests/test_cmetta]. */
#define MT_DEEP        400000   /* mt_eq and mt_drop, which stay in C */
#define MT_DEEP_ENGINE 100000   /* encode, decode and mt_bound, which cross */

static mt_atom *nested(size_t depth)
{ mt_atom *a = S("leaf");
  size_t i;
  for (i = 0; i < depth && a; i++) a = E("f", a);
  return a;
}

static void test_a_deep_term_does_not_overrun_the_stack(metta *m)
{ mt_atom *deep = nested(MT_DEEP);
  mt_space *kb;
  int matched = 0;

  CASE("a term nested deeper than the C stack is compared, written, read and released");
  CHECK(deep != NULL);
  CHECK(mt_kind_of(deep) == MT_EXPR);
  mt_clear();

  /* mt_eq. The twin goes as soon as it has been compared: one of these is
     25 MB, and the case is about depth rather than about how many fit. */
  { mt_atom *twin = nested(MT_DEEP);
    CHECK(twin != NULL);
    CHECK(mt_eq(deep, twin));
    mt_drop(twin);
  }

  /* encode and decode, through the engine's own writer and reader, at the
     depth the crossing pays for. */
  { mt_atom *crossing = nested(MT_DEEP_ENGINE);
    char *written = mt_show_dup(crossing);
    mt_atom *read_back;
    CHECK(written != NULL);
    read_back = mt_parse(written);
    CHECK(read_back != NULL);
    CHECK(mt_eq(crossing, read_back));
    mt_free(written);
    mt_drop(read_back);
    mt_drop(crossing);
  }
  CHECK(mt_ok());

  /* bound_in, which walks a deep pattern against a deep answer. */
  kb = mt_space_open(m, "&cmetta-deep");
  CHECK(kb != NULL);
  CHECK(mt_add(kb, nested(MT_DEEP_ENGINE)));
  { mt_atom *pattern = V("x");
    size_t i;
    for (i = 0; i < MT_DEEP_ENGINE; i++) pattern = E("f", pattern);
    mt_rows (row, mt_match(kb, pattern))
    { CHECK(mt_kind_of(mt_bound(row, "x")) == MT_SYMBOL);
      matched++;
      break;
    }
  }
  CHECK(matched == 1);
  CHECK(mt_wipe(kb));
  mt_space_close(kb);

  /* mt_drop last, because it is the walk that cannot answer "no". */
  mt_drop(deep);
  CHECK(mt_ok());
}

/* metta_c_close/1 runs from mt_answers_free() whatever ended the walk, and a
   cursor that reached the end of its answers has an engine that answered its
   last. Closing that must leave the error state clean, because a host reading
   mt_ok() after a loop is reading the loop's verdict. */
static void test_closing_an_exhausted_cursor_is_quiet(metta *m)
{ mt_answers *cursor;
  int pulled = 0;

  CASE("a cursor walked to exhaustion closes without a word");
  mt_clear();
  cursor = mt_eval(m, E("superpose", E(1, 2, 3)));
  CHECK(cursor != NULL);
  while ( mt_next(cursor) ) pulled++;
  CHECK(pulled == 3);
  CHECK(mt_ok());          /* exhaustion is not a failure */
  mt_answers_free(cursor);
  CHECK(mt_ok());          /* and neither is the close that follows it */

  /* The runtime is unharmed, which is the other half of the claim. */
  CHECK(mt_one_int(mt_eval(m, E("+", 1, 2))) == 3);
  CHECK(mt_ok());
}

/* The engine's four-call host protocol proves the name is free BEFORE the
   binding asserts anything, so a name another tier owns is refused rather
   than clobbered. `is` is Prolog's own at this arity. */
static mt_status op_never_called(mt_call *call, void *user)
{ (void)user;
  return mt_fail(call, "this operation should never have been registered");
}

static void test_a_taken_name_is_refused_rather_than_clobbered(metta *m)
{ CASE("publishing over a name another tier owns is refused, with the owner named");
  mt_clear();
  CHECK(mt_def(m, (mt_op){ .name = "is", .arity = 1, .effect = MT_PURE,
                           .fn = op_never_called }) == false);
  CHECK(!mt_ok());
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "is/2") != NULL);

  /* Refused before any write, so the name is still Prolog's and the engine
     still runs. */
  mt_clear();
  CHECK(mt_one_int(mt_eval(m, E("+", 2, 3))) == 5);
  CHECK(mt_ok());
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

static mt_status op_answer_nothing(mt_call *call, void *user)
{ (void)call;
  (void)user;
  return MT_OK;
}

typedef struct callback_probe
{ metta *runtime;
  bool saw_runtime;
  mt_status first_answer;
  mt_status second_answer;
} callback_probe;

static mt_status op_report_runtime(mt_call *call, void *user)
{ callback_probe *probe = user;
  probe->saw_runtime = mt_of(call) == probe->runtime;
  return mt_answer(call, B(probe->saw_runtime));
}

static mt_status op_answer_twice(mt_call *call, void *user)
{ callback_probe *probe = user;
  probe->first_answer = mt_answer(call, N(1));
  probe->second_answer = mt_answer(call, N(2));
  return probe->second_answer;
}

static mt_status op_fail_silently_after_new_error(mt_call *call, void *user)
{ (void)call;
  (void)user;
  mt_drop(mt_bigint("fresh-operation-error"));
  return MT_OK;
}

static void test_an_answerless_operation_uses_only_its_own_error(metta *m)
{ CASE("an answerless operation does not report a stale errno-shaped failure");
  CHECK(mt_def(m, (mt_op){ .name = "answer-nothing", .arity = 0,
                           .effect = MT_PURE, .fn = op_answer_nothing }));
  mt_drop(mt_bigint("stale-before-operation"));
  CHECK(mt_run(m, "!(answer-nothing)") == NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "answered nothing") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "stale-before-operation") == NULL);

  CASE("a new callback failure is used even when its status matches the stale one");
  CHECK(mt_def(m, (mt_op){ .name = "answer-new-error", .arity = 0,
                           .effect = MT_PURE,
                           .fn = op_fail_silently_after_new_error }));
  mt_drop(mt_bigint("another-stale-error"));
  CHECK(mt_run(m, "!(answer-new-error)") == NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "fresh-operation-error") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "another-stale-error") == NULL);

  CHECK(mt_undef(m, "answer-nothing"));
  CHECK(mt_undef(m, "answer-new-error"));
}

static void test_a_c_function_is_callable_from_metta(metta *m)
{ callback_probe probe = { .runtime = m };

  CASE("a published C function answers a MeTTa call");
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

  CASE("a live callback can recover the runtime that invoked it");
  CHECK(mt_def(m, (mt_op){ .name = "callback-runtime", .arity = 0,
                           .effect = MT_PURE, .fn = op_report_runtime,
                           .user = &probe }));
  CHECK(mt_one_truth(mt_run(m, "!(callback-runtime)")));
  CHECK(probe.saw_runtime);
  CHECK(mt_undef(m, "callback-runtime"));

  CASE("a callback cannot answer one application twice");
  CHECK(mt_def(m, (mt_op){ .name = "answer-twice", .arity = 0,
                           .effect = MT_PURE, .fn = op_answer_twice,
                           .user = &probe }));
  mt_clear();
  CHECK(mt_run(m, "!(answer-twice)") == NULL);
  CHECK(probe.first_answer == MT_OK);
  CHECK(probe.second_answer == MT_MISUSE);
  CHECK(mt_error() == MT_ERROR);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "already answered"));
  CHECK(mt_undef(m, "answer-twice"));
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
#define CMETTA_RAW_LIMIT         17
#define CMETTA_RAW_FN(x)         ((x) + 1)

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

static void test_raw_lowering_preserves_tokens_that_are_c_macros(metta *m)
{ const char *raw = MT_METTA_RAW((CMETTA_RAW_LIMIT CMETTA_RAW_FN(1)));
  mt_atom *equation;
  const mt_atom *body;

  CASE("MT_METTA_RAW stringifies object-like and function-like macros literally");
  CHECK(strcmp(raw, "(CMETTA_RAW_LIMIT CMETTA_RAW_FN(1))") == 0);
  CHECK(strcmp(MT_METTA((CMETTA_RAW_LIMIT)), "(17)") == 0);

  CASE("mt_lower_raw stores the literal MeTTa symbol rather than its C expansion");
  CHECK(mt_lower_raw(m, (cmetta-raw-collision),
                     (quote CMETTA_RAW_LIMIT)));
  equation = mt_one(mt_match(m,
      E("=", E("cmetta-raw-collision"), V("body"))));
  CHECK(equation != NULL);
  body = equation ? mt_at(equation, 2) : NULL;
  CHECK(body && mt_kind_of(body) == MT_EXPR);
  CHECK(body && mt_len(body) == 2);
  CHECK(body && strcmp(mt_name(mt_at(body, 1)), "CMETTA_RAW_LIMIT") == 0);
  mt_drop(equation);
}

static void test_a_c_value_crosses_by_reference(metta *m)
{ static counter c = {0};
  mt_atom *handle;
  mt_space *space;
  int matched = 0;

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

  CASE("the same C object is one engine identity across store, match and delete");
  space = mt_space_open(m, "&cmetta-object-identity");
  CHECK(space != NULL);
  CHECK(mt_add(space, mt_keep(handle)));
  CHECK(mt_count(space) == 1);
  mt_each (found, mt_match(space, mt_keep(handle)))
  { CHECK(mt_value(found) == &c);
    matched++;
  }
  CHECK(matched == 1);
  CHECK(mt_del(space, mt_keep(handle)));
  CHECK(mt_count(space) == 0);
  mt_space_close(space);

  mt_drop(handle);
  CHECK(mt_undef(m, "bump"));
}

static void test_an_object_can_be_released_without_waiting_for_atom_gc(metta *m)
{ release_probe probe = {0};
  mt_space *space;
  mt_answers *answers;
  mt_atom *handle;

  CASE("mt_object_free releases a crossed object immediately");
  space = mt_space_open(m, "&cmetta-explicit-release");
  handle = mt_object(&probe, "release-probe", count_release);
  CHECK(space != NULL);
  CHECK(handle != NULL);
  CHECK(mt_add(space, mt_keep(handle)));
  CHECK(probe.calls == 0);
  CHECK(mt_object_free(handle));
  CHECK(probe.calls == 1);

  CASE("an engine alias left by explicit release is refused without a dereference");
  mt_clear();
  answers = mt_match(space, V("x"));
  CHECK(answers != NULL);
  CHECK(mt_next(answers) == NULL);
  CHECK(mt_error() == MT_UNSUPPORTED);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "explicitly released") != NULL);
  mt_answers_free(answers);
  mt_clear();
  CHECK(mt_space_wipe(space));
  mt_space_close(space);
}

static void test_float_identity_agrees_with_the_engine(metta *m)
{ mt_atom *positive_zero = R(0.0);
  mt_atom *negative_zero = R(-0.0);
  mt_atom *nan_a = R(nan("1"));
  mt_atom *nan_b = R(-nan("42"));
  mt_atom *finite_a = R(1.5);
  mt_atom *finite_b = R(1.5);
  mt_space *space = mt_space_open(m, "&cmetta-float-identity");

  CASE("float structural equality distinguishes signed zero");
  CHECK(!mt_eq(positive_zero, negative_zero));
  CHECK(mt_add(space, mt_keep(positive_zero)));
  CHECK(!mt_del(space, mt_keep(negative_zero)));
  CHECK(mt_del(space, mt_keep(positive_zero)));

  CASE("all NaN payloads share the engine's canonical structural identity");
  CHECK(mt_eq(nan_a, nan_b));
  CHECK(mt_add(space, mt_keep(nan_a)));
  CHECK(mt_del(space, mt_keep(nan_b)));
  CHECK(mt_count(space) == 0);

  CASE("ordinary equal floats remain structurally identical");
  CHECK(mt_eq(finite_a, finite_b));
  CHECK(mt_add(space, mt_keep(finite_a)));
  CHECK(mt_del(space, mt_keep(finite_b)));

  mt_drop(positive_zero);
  mt_drop(negative_zero);
  mt_drop(nan_a);
  mt_drop(nan_b);
  mt_drop(finite_a);
  mt_drop(finite_b);
  mt_space_close(space);
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
{ release_probe release = {0};
  mt_atom *fn;

  CASE("a C function carried as a value is applied where it lands");
  fn = mt_function(fn_triple, NULL, NULL);
  CHECK(fn != NULL);
  CHECK(mt_one_int(mt_eval(m, E(mt_keep(fn), 5))) == 15);

  CASE("a function value's release callback runs once on explicit release");
  mt_drop(fn);
  fn = mt_function(fn_triple, &release, count_release);
  CHECK(fn != NULL);
  CHECK(mt_one_int(mt_eval(m, E(mt_keep(fn), 7))) == 21);
  CHECK(mt_object_free(fn));
  CHECK(release.calls == 1);
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

static void test_a_refused_stack_limit_clears_the_engine_exception(metta *m)
{ mt_limits old = mt_limits_of(m);
  mt_limits refused = old;

  CASE("a refused stack limit reports its exception and leaves none pending");
  refused.stack_bytes = 1;
  mt_clear();
  CHECK(!mt_limit(m, refused));
  CHECK(mt_error() == MT_ERROR);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "stack") != NULL);
  CHECK(PL_exception(0) == 0);
  CHECK(mt_limits_of(m).seconds == old.seconds);
  CHECK(mt_limits_of(m).inferences == old.inferences);
  CHECK(mt_limits_of(m).stack_bytes == old.stack_bytes);

  CASE("the host can call the engine after the caught exception");
  mt_clear();
  CHECK(mt_limit(m, old));
  CHECK(mt_one_int(mt_run(m, "!(+ 1 2)")) == 3);
  CHECK(mt_ok());
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

  CASE("a wall bound stops an eager call that does not finish");
  bounded.inferences = 0;
  bounded.seconds = 0.001;
  CHECK(mt_limit(m, bounded));
  mt_clear();
  CHECK(mt_run(m, "!(from 0)") == NULL);
  CHECK(mt_error() == MT_LIMIT);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "second") != NULL);

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
{ metta *m;

  /* Before the runtime exists, because that is the only moment its absence
     can be asked about. */
  test_a_door_before_the_runtime_refuses();
  test_an_uncrossed_object_can_be_released_without_an_engine();

  if ( !(m = mt_open(NULL)) )
  { fprintf(stderr, "cannot boot the engine: %s\n", mt_errmsg());
    return 1;
  }

  test_atoms_need_no_engine();
  test_public_scalar_readers_cover_their_whole_domain();
  test_the_builder_coerces_each_child_by_its_c_type();
  test_a_macro_evaluates_each_argument_exactly_once(m);
  test_a_failed_child_does_not_leak_its_siblings();
  test_refusals_are_named();
  test_a_failed_constructor_says_so();
  test_a_ratio_is_stored_in_canonical_form();
  test_a_ratio_is_canonical_in_both_halves(m);
  test_reading_promotes_only_where_it_is_lossless();
  test_the_error_state_is_errno_shaped();
  test_reference_counting_holds_under_churn();
  test_text_crosses_through_the_engine_reader();
  test_presentation_and_round_trip_text_are_distinct();
  test_run_groups_answers_by_form(m);
  test_the_walk_closes_its_cursor_on_break(m);
  test_one_and_first_make_different_claims(m);
  test_spaces_store_and_query(m);
  test_catalog_and_file_load_are_live_runtime_doors(m);
  test_engine_owned_base_spaces_refuse_wipe(m);
  test_a_door_that_takes_an_atom_refuses_null(m);
  test_a_deep_term_does_not_overrun_the_stack(m);
  test_closing_an_exhausted_cursor_is_quiet(m);
  test_a_taken_name_is_refused_rather_than_clobbered(m);
  test_one_verb_takes_either_receiver(m);
  test_a_user_space_decodes_as_a_space(m);
  test_a_c_function_is_callable_from_metta(m);
  test_an_answerless_operation_uses_only_its_own_error(m);
  test_a_c_body_lowers_into_an_equation_the_engine_can_see(m);
  test_raw_lowering_preserves_tokens_that_are_c_macros(m);
  test_a_c_value_crosses_by_reference(m);
  test_an_object_can_be_released_without_waiting_for_atom_gc(m);
  test_float_identity_agrees_with_the_engine(m);
  test_a_function_value_is_applicable(m);
  test_an_engine_error_reaches_c_as_words(m);
  test_a_refused_stack_limit_clears_the_engine_exception(m);
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
