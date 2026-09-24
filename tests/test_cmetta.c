/* Purpose: exercise every door of the C binding against a live engine, and
 *   fail loudly on the first one that does not behave as cmetta.h says.
 * Assumes: one runtime per process, so every case shares one engine and a
 *   case that writes to &self cleans up after itself.
 * Guarantees: exits 0 only when every case passed; prints the failing
 *   expression, its file and its line otherwise.
 *   Engine-owned &self and &metta refuse wipe without damaging catalog,
 *   typing, or arithmetic state; an ordinary named space still wipes
 *   [tested: test_engine_owned_base_spaces_refuse_wipe; commit=6229e43cb68cc3685360810d462d992874992f6c].
 *   mt_compare orders numbers of any width as the engine's msort does, a
 *   BigRational and an 800-digit BigInt among them, past the 2560 bits a
 *   fixed-width compare once refused [tested:
 *   test_the_standard_order_is_the_engines; commit=23bce3e95153812edb34f405e5f13788119ef7d1].
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#define MT_SHORTHAND
#include <cmetta.h>
#include <SWI-Prolog.h>

#include <math.h>
#include <stdlib.h>
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

  CASE("every public status has one stable name");
  for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++)
    CHECK(strcmp(mt_status_str((mt_status)i), statuses[i]) == 0);
  CHECK(strcmp(mt_status_str((mt_status)99), "unknown status") == 0);
  CHECK(strcmp(mt_version(), MT_VERSION) == 0);

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
   mt_stats_now, each a SIGSEGV a host cannot catch
   [measured 2026-08-31; C34 in ai-cmetta-c-constraints.md]. The case
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
  CHECK(mt_run((metta *)NULL, "!(+ 1 2)") == NULL);
  CHECK(mt_do((metta *)NULL, "(= (f) 1)") == false);
  CHECK(mt_load((metta *)NULL, "/nonexistent.metta") == NULL);
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
   answering true with the error state clean
   [measured 2026-08-31; C32 in ai-cmetta-c-constraints.md]. */
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
   2026-08-31 on this box's 8 MB thread stack; C35 in
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
  CHECK(mt_def(m, (mt_op){ .name = "is", .arity = 1, .effect = MT_EFFECT_CLASS_PURE_STRUCTURAL,
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
                           .effect = MT_EFFECT_CLASS_PURE_STRUCTURAL, .fn = op_answer_nothing }));
  mt_drop(mt_bigint("stale-before-operation"));
  CHECK(mt_run(m, "!(answer-nothing)") == NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "answered nothing") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "stale-before-operation") == NULL);

  CASE("a new callback failure is used even when its status matches the stale one");
  CHECK(mt_def(m, (mt_op){ .name = "answer-new-error", .arity = 0,
                           .effect = MT_EFFECT_CLASS_PURE_STRUCTURAL,
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
                                 .effect = MT_EFFECT_CLASS_PURE_STRUCTURAL, .fn = op_double }));
  CHECK(mt_one_int(mt_run(m, "!(cdouble 21)")) == 42);

  CASE("a published name preserves underscores and remains distinct from hyphens");
  CHECK(mt_def(m, (mt_op){ .name = "tag_it", .arity = 1,
                                 .effect = MT_EFFECT_CLASS_PURE_STRUCTURAL, .fn = op_tag_it,
                                 .user = (void *)"tagged" }));
  { mt_atom *got = mt_one(mt_run(m, "!(tag_it 7)"));
    CHECK(got && mt_kind_of(got) == MT_EXPR);
    CHECK(mt_len(got) == 2);
    CHECK(strcmp(mt_name(mt_at(got, 0)), "tagged") == 0);
    mt_drop(got);
  }
  { mt_atom *got = mt_one(mt_run(m, "!(tag-it 7)"));
    mt_atom *unreduced = E("tag-it", 7);
    CHECK(mt_eq(got, unreduced));
    mt_drop(got); mt_drop(unreduced);
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
                                  .effect = (enum mt_effect_class)99, .fn = op_double }));
  CHECK(mt_error() == MT_MISUSE);

  CASE("a withdrawn name is data again");
  CHECK(mt_undef(m, "cdouble"));
  mt_each (a, mt_run(m, "!(cdouble 21)"))
      CHECK(mt_kind_of(a) == MT_EXPR);
  CHECK(mt_undef(m, "tag_it"));

  CASE("a live callback can recover the runtime that invoked it");
  CHECK(mt_def(m, (mt_op){ .name = "callback-runtime", .arity = 0,
                           .effect = MT_EFFECT_CLASS_PURE_STRUCTURAL, .fn = op_report_runtime,
                           .user = &probe }));
  CHECK(mt_one_truth(mt_run(m, "!(callback-runtime)")));
  CHECK(probe.saw_runtime);
  CHECK(mt_undef(m, "callback-runtime"));

  CASE("a callback cannot answer one application twice");
  CHECK(mt_def(m, (mt_op){ .name = "answer-twice", .arity = 0,
                           .effect = MT_EFFECT_CLASS_PURE_STRUCTURAL, .fn = op_answer_twice,
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
                                 .effect = MT_EFFECT_CLASS_WRITES_STATE, .fn = op_bump }));
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
  /* The report is THREE lines, and the two under the headline are the answers
     that differed. They are continuation lines of one print_message/2 message,
     so a capture window that took the headline and dropped the rest would
     still satisfy the check above and lose the whole diagnosis. */
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "missing: (2)") != NULL);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "excess: (1)") != NULL);

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

/* Two children of one answer: whether they are one variable, by the name C
   reads a variable's identity from. */
static bool one_variable(const mt_atom *a, const mt_atom *b)
{ return mt_kind_of(a) == MT_VARIABLE && mt_kind_of(b) == MT_VARIABLE &&
         strcmp(mt_name(a), "_") != 0 && mt_eq(a, b);
}

static void test_an_answer_keeps_variable_identity(metta *m)
{ mt_space *kb, *copy;
  mt_atom *first, *second;

  CASE("a stored atom reads back with one name per variable");
  mt_clear();
  kb = mt_space_open(m, "&cmetta-vars");
  CHECK(kb != NULL);
  CHECK(mt_add(kb, E("fact", V("u"), V("u"), V("w"))));
  mt_each (a, mt_atoms(kb))
  { CHECK(one_variable(mt_at(a, 1), mt_at(a, 2)));
    CHECK(!mt_eq(mt_at(a, 1), mt_at(a, 3)));
    CHECK(mt_kind_of(mt_at(a, 3)) == MT_VARIABLE &&
          strcmp(mt_name(mt_at(a, 3)), "_") != 0);
  }
  mt_each (a, mt_match(kb, E("fact", V("p"), V("q"), V("r"))))
  { CHECK(one_variable(mt_at(a, 1), mt_at(a, 2)));
    CHECK(!mt_eq(mt_at(a, 1), mt_at(a, 3)));
  }

  CASE("two crossings never share a fresh name");
  first = mt_one(mt_eval(m, E("pair", V("z"), V("z"))));
  second = mt_one(mt_eval(m, E("pair", V("z"), V("z"))));
  CHECK(first && second);
  if ( first && second )
  { CHECK(one_variable(mt_at(first, 1), mt_at(first, 2)));
    CHECK(one_variable(mt_at(second, 1), mt_at(second, 2)));
    CHECK(!mt_eq(mt_at(first, 1), mt_at(second, 1)));
  }
  mt_drop(first);
  mt_drop(second);

  CASE("an equation read back through C still computes where it is copied");
  /* Before variables kept their identity, (= (sq $x) (* $x $x)) read back as
     three unrelated variables, and copying it into another space stored an
     equation whose body no longer mentioned its argument. */
  CHECK(mt_add(kb, E("=", E("sq", V("x")), E("*", V("x"), V("x")))));
  copy = mt_space_open(m, "&cmetta-vars-copy");
  CHECK(copy != NULL);
  mt_each (eq, mt_match(kb, E("=", E("sq", V("arg")), V("body"))))
    CHECK(mt_add(copy, mt_keep(eq)));
  CHECK(mt_one_int(mt_eval(copy, E("sq", 7))) == 49);
  CHECK(mt_ok());

  CHECK(mt_wipe(kb));
  CHECK(mt_wipe(copy));
  mt_space_close(copy);
  mt_space_close(kb);
}

/* xorshift64, so a failing generated case names a seed that reproduces it
   [source: Marsaglia, "Xorshift RNGs", Journal of Statistical Software 8(14),
   2003]. */
static uint64_t alpha_rng = UINT64_C(0x9e3779b97f4a7c15);
static unsigned alpha_draw(unsigned bound)
{ alpha_rng ^= alpha_rng << 13;
  alpha_rng ^= alpha_rng >> 7;
  alpha_rng ^= alpha_rng << 17;
  return (unsigned)(alpha_rng % bound);
}

/* A small random term over two symbols, two numbers and the variables $x,
   $y, $z and the anonymous $_, so variants and near-variants are common. */
static mt_atom *alpha_term(unsigned depth, const char *const names[4])
{ unsigned pick = alpha_draw(depth ? 9 : 7);
  switch ( pick )
  { case 0: return S("a");
    case 1: return S("b");
    case 2: return N(1);
    case 3: case 4: case 5: case 6: return V(names[pick - 3]);
    default:
    { mt_atom *kids[3];
      unsigned n = alpha_draw(4), i;
      for (i = 0; i < n; i++) kids[i] = alpha_term(depth - 1, names);
      return mt_exprv(n, kids);
    }
  }
}

static void test_alpha_equivalence_is_a_renaming(metta *m)
{ static const char *const plain[4] = { "x", "y", "z", "_" };
  static const char *const renamed[4] = { "q", "r", "p", "_" };
  static const char *const merged[4] = { "x", "x", "z", "_" };
  unsigned i, agreed = 0, variants = 0;

  CASE("alpha equivalence renames variables consistently and one-to-one");
  { mt_atom *xy = E("f", V("x"), V("y")), *ab = E("f", V("a"), V("b")),
            *aa = E("f", V("a"), V("a")), *xx = E("f", V("x"), V("x")),
            *anon = E("f", V("_"), V("_")),
            *nested = E("g", E("h", V("x")), V("x")),
            *nested2 = E("g", E("h", V("y")), V("y")),
            *split = E("g", E("h", V("y")), V("z")),
            *ints = E("f", 1, 2), *mixed = E("f", 1, 2.0);
    CHECK(mt_alpha_eq(xy, ab));
    CHECK(!mt_alpha_eq(xy, aa));
    CHECK(!mt_alpha_eq(aa, xy));
    CHECK(mt_alpha_eq(xx, aa));
    CHECK(mt_alpha_eq(anon, ab));
    CHECK(!mt_alpha_eq(anon, aa));
    CHECK(mt_alpha_eq(nested, nested2));
    CHECK(!mt_alpha_eq(nested, split));
    CHECK(!mt_alpha_eq(ints, mixed));
    CHECK(mt_alpha_eq(ints, ints));
    CHECK(!mt_alpha_eq(xy, NULL) && !mt_alpha_eq(NULL, NULL));
    mt_drop(xy); mt_drop(ab); mt_drop(aa); mt_drop(xx); mt_drop(anon);
    mt_drop(nested); mt_drop(nested2); mt_drop(split); mt_drop(ints);
    mt_drop(mixed);
  }

  CASE("alpha equivalence agrees with the engine's =alpha on generated pairs");
  mt_clear();
  for (i = 0; i < 400; i++)
  { uint64_t seed = alpha_rng;
    mt_atom *a, *b;
    bool here, there;
    /* The same draws twice over two name tables give a term and a renaming
       of it; a third table merges two variables; a fresh draw is unrelated. */
    a = alpha_term(3, plain);
    switch ( i % 3 )
    { case 0: alpha_rng = seed; b = alpha_term(3, renamed); break;
      case 1: alpha_rng = seed; b = alpha_term(3, merged); break;
      default: b = alpha_term(3, plain); break;
    }
    here = mt_alpha_eq(a, b);
    there = mt_one_truth(mt_eval(m, E("=alpha", mt_keep(a), mt_keep(b))));
    if ( here != there )
      fprintf(stderr, "alpha disagreement at seed %llu: %s vs %s, C %d, "
              "engine %d\n", (unsigned long long)seed, mt_show(a),
              mt_show(b), here, there);
    agreed += here == there;
    variants += here;
    mt_drop(a);
    mt_drop(b);
  }
  CHECK(agreed == 400);
  CHECK(variants > 40 && variants < 360);   /* both answers were exercised */
  CHECK(mt_ok());
}

/* An 800-digit integer, about 2,658 bits: a one then zeros, or nines, with
   the sign bit of `which` negating it. */
static mt_atom *very_wide(unsigned which)
{ char text[802];
  size_t at = 0;
  if ( which & 2 ) text[at++] = '-';
  text[at++] = which & 1 ? '9' : '1';
  memset(text + at, which & 1 ? '9' : '0', 799);
  text[at + 799] = '\0';
  return mt_bigint(text);
}

/* A random atom of every kind the standard order ranks, drawn so that equal
   values of different numeric kinds and shared prefixes are common. */
static mt_atom *order_atom(unsigned depth)
{ static const char *const symbols[] = { "a", "b", "Apple", "zeta", "\xc3\xa9" };
  static const char *const texts[] = { "a", "B", "text", "" };
  static const char *const wide[] = { "99999999999999999999", "-99999999999999999999",
                                      "18446744073709551616", "9223372036854775808" };
  static const double floats[] = { 0.0, -0.0, 0.5, 2.0, -1.5, 1e19, 1e20,
                                   9007199254740996.0, 0.3333333333333333,
                                   6.223015277861142e-61 };
  /* 2^-200 is exactly the last float above, so a tie between kinds is drawn. */
  static const char *const ratios[] = {
    "1/1606938044258990275541962092341162602522202993782792835301376",
    "-1/1606938044258990275541962092341162602522202993782792835301376",
    "1606938044258990275541962092341162602522202993782792835301377/2",
    "9223372036854775808/3" };
  switch ( alpha_draw(depth ? 13 : 12) )
  { case 0: return N((int64_t)alpha_draw(5) - 2);
    case 1: return N(alpha_draw(2) ? INT64_MAX : INT64_MIN);
    case 2: return R(floats[alpha_draw(10)]);
    case 3: return R(NAN);
    case 4: return mt_bigint(wide[alpha_draw(4)]);
    case 5: return mt_rational((int64_t)alpha_draw(7) - 3, (int64_t)alpha_draw(3) + 2);
    case 6: return T(texts[alpha_draw(4)]);
    case 7: return S(symbols[alpha_draw(5)]);
    case 8: return B(alpha_draw(2) != 0);
    case 9: return mt_unit();
    case 10: return mt_bigrational(ratios[alpha_draw(4)]);
    case 11: return very_wide(alpha_draw(4));
    default:
    { mt_atom *kids[3];
      unsigned n = alpha_draw(3) + 1, i;
      for (i = 0; i < n; i++) kids[i] = order_atom(depth - 1);
      return mt_exprv(n, kids);
    }
  }
}

static void test_the_standard_order_is_the_engines(metta *m)
{ unsigned round, agreed = 0, pairs = 0;

  CASE("mixed numbers compare by exact value, a float first on a tie");
  { mt_atom *two = N(2), *two_f = R(2.0), *half = mt_rational(1, 2), *half_f = R(0.5),
            *big = mt_bigint("99999999999999999999"), *e19 = R(1e19), *e20 = R(1e20),
            *exact = N(9007199254740995), *above = R(9007199254740996.0),
            *nan = R(NAN), *neg0 = R(-0.0), *pos0 = R(0.0), *zero = N(0);
    CHECK(mt_compare(two_f, two) < 0 && mt_compare(two, two_f) > 0);
    CHECK(mt_compare(half_f, half) < 0);
    CHECK(mt_compare(e19, big) < 0 && mt_compare(big, e20) < 0);
    CHECK(mt_compare(exact, above) < 0);   /* as floats they would tie */
    CHECK(mt_compare(nan, zero) < 0 && mt_compare(nan, nan) == 0);
    CHECK(mt_compare(neg0, pos0) < 0 && mt_compare(pos0, zero) < 0);
    mt_drop(two); mt_drop(two_f); mt_drop(half); mt_drop(half_f); mt_drop(big);
    mt_drop(e19); mt_drop(e20); mt_drop(exact); mt_drop(above); mt_drop(nan);
    mt_drop(neg0); mt_drop(pos0); mt_drop(zero);
  }

  CASE("numbers of any width compare exactly");
  { mt_atom *wide = very_wide(1), *one = N(1), *below = very_wide(3),
            *tiny = mt_bigrational("1/1606938044258990275541962092341162602522202993782792835301376"),
            *tiny_f = R(ldexp(1.0, -200)),
            *huge = mt_bigrational("1606938044258990275541962092341162602522202993782792835301376/1");
    mt_clear();
    CHECK(mt_compare(wide, one) > 0 && mt_compare(one, wide) < 0);
    CHECK(mt_compare(below, wide) < 0 && mt_compare(below, below) == 0);
    CHECK(mt_compare(tiny_f, tiny) < 0 && mt_compare(tiny, tiny_f) > 0);  /* equal: the float first */
    CHECK(mt_compare(tiny, huge) < 0 && mt_compare(huge, wide) < 0);
    CHECK(mt_ok());
    mt_drop(wide); mt_drop(one); mt_drop(below); mt_drop(tiny); mt_drop(tiny_f); mt_drop(huge);
  }

  CASE("the classes rank as the engine ranks them, and qsort takes mt_order");
  { mt_atom *items[] = { E("x", 1), mt_unit(), S("zeta"), T("text"), N(3), R(2.5),
                         B(true), S("Apple") };
    size_t n = sizeof items / sizeof items[0], i;
    qsort(items, n, sizeof items[0], mt_order);
    CHECK(mt_kind_of(items[0]) == MT_FLOAT && mt_kind_of(items[1]) == MT_INT);
    CHECK(mt_kind_of(items[2]) == MT_TEXT && mt_len(items[3]) == 0 &&
          mt_kind_of(items[3]) == MT_EXPR);
    CHECK(strcmp(mt_name(items[4]), "Apple") == 0 && mt_kind_of(items[5]) == MT_BOOL);
    CHECK(strcmp(mt_name(items[6]), "zeta") == 0 && mt_len(items[7]) == 2);
    for (i = 0; i < n; i++) mt_drop(items[i]);
  }

  CASE("the engine's msort never answers two atoms mt_compare would swap");
  mt_clear();
  for (round = 0; round < 40; round++)
  { mt_atom *kids[24];
    mt_atom *sorted;
    size_t i;
    for (i = 0; i < 24; i++) kids[i] = order_atom(2);
    sorted = mt_one(mt_eval(m, E("msort", mt_exprv(24, kids))));
    if ( !sorted || mt_len(sorted) != 24 )
    { CHECK(!"msort answered a list of the same length");
      mt_drop(sorted);
      continue;
    }
    for (i = 0; i + 1 < 24; i++)
    { const mt_atom *x = mt_at(sorted, i), *y = mt_at(sorted, i + 1);
      int order = mt_compare(x, y);
      /* A tie is only right between atoms the engine cannot tell apart. */
      bool fine = order < 0 || (order == 0 && (mt_eq(x, y) ||
                  mt_kind_of(x) == MT_BOOL || mt_kind_of(y) == MT_BOOL));
      if ( !fine )
        fprintf(stderr, "order disagreement: engine put %s before %s\n",
                mt_show(x), mt_show(y));
      agreed += fine;
      pairs++;
    }
    mt_drop(sorted);
  }
  CHECK(pairs == 40 * 23 && agreed == pairs);
  CHECK(mt_ok());
}

/* A partial application answers as the expression the shared wire grammar
   gives it, (partial F Args), which is the atom a C program builds and the one
   the Python and Node seats answer; as there, passing it back does not apply
   it [source: docs/journal/2026-09-05-node-runtime-gaps.md;
   commit=b88bfb4ce75e4f37ccda3d99456acb40afddf761]. */
static void test_a_partial_application_answers_as_its_wire_expression(metta *m)
{ mt_atom *partial, *built, *both, *want, *held;
  mt_space *kb;
  size_t rows = 0;

  CASE("a partial application answers as (partial F Args)");
  mt_clear();
  partial = mt_one(mt_eval(m, E("id", E("+", 1))));
  built = E("partial", "+", E(1));
  CHECK(partial != NULL && mt_ok());
  CHECK(mt_kind_of(partial) == MT_EXPR);
  CHECK(partial && mt_eq(partial, built));
  CHECK(partial && strcmp(mt_show(partial), "(partial + (1))") == 0);

  CASE("two partials collapse as the Node seat prints them");
  both = mt_one(mt_eval(m, E("collapse", E("superpose", E(E("*", 2), E("+", 3))))));
  want = E(E("partial", "*", E(2)), E("partial", "+", E(3)));
  CHECK(both && mt_eq(both, want));
  mt_drop(both);
  mt_drop(want);

  CASE("stored and matched back, it is the same expression");
  kb = mt_space_open(m, "&cmetta-partials");
  CHECK(kb && mt_add(kb, E("holds", mt_keep(partial))));
  held = mt_first(mt_match(kb, E("holds", V("f"))));
  CHECK(held && mt_eq(mt_at(held, 1), built));
  mt_drop(held);
  CHECK(mt_wipe(kb));
  mt_space_close(kb);

  CASE("a run whose answer is a partial answers every group");
  mt_clear();
  mt_rows (row, mt_run(m, "!(id (+ 1)) !(+ 1 1)")) rows++;
  CHECK(rows == 2 && mt_ok());
  mt_drop(partial);
  mt_drop(built);
  mt_clear();
}

/* A caught refusal carries an engine compound as its payload, and C reads it
   apart by position, as index-atom does in the engine. */
static void test_a_refusal_payload_is_an_expression(metta *m)
{ mt_space *kb = mt_space_open(m, "&cmetta-fence");
  mt_atom *refusal;
  const mt_atom *payload;

  CASE("a caught refusal's payload is an expression C reads by position");
  CHECK(kb && mt_add(kb, E("Order", 7, "x", "y")));
  refusal = mt_one(mt_eval(m, E("catch", E("match", mt_spaceref("&cmetta-fence"),
                                              E("Order", E(":seg", V("m")), V("m")), "hit"))));
  payload = refusal && mt_len(refusal) > 1 ? mt_at(refusal, 1) : NULL;
  CHECK(payload && mt_kind_of(payload) == MT_EXPR);
  CHECK(payload && mt_name(mt_at(payload, 0)) &&
        strcmp(mt_name(mt_at(payload, 0)), "metta_seq_outside_fragment") == 0);
  CHECK(payload && mt_name(mt_at(payload, 4)) && strcmp(mt_name(mt_at(payload, 4)), "mixed_roles") == 0);
  mt_drop(refusal);
  CHECK(kb && mt_wipe(kb));
  mt_space_close(kb);
  mt_clear();
}

/* Naming a parse's variables once kept two term references per name pair for
   every variable, v times the list's length, and handed the 0 the stacks
   answered once they filled to PL_get_arg, which aborts the process: 3,000
   variables under a 16 MB stack limit did [measured 2026-09-24]. */
static void test_many_variables_are_named_without_aborting(metta *m)
{ enum { COUNT = 3000 };
  mt_limits old = mt_limits_of(m), tight = old;
  char *source = malloc(COUNT * 8 + 8), *at = source;
  char want[16];
  mt_atom *parsed;
  int i, named = 0;

  CASE("a parse of 3,000 distinct variables names each one under a 16 MB stack");
  CHECK(source != NULL);
  if ( !source ) return;
  at += sprintf(at, "(f");
  for (i = 0; i < COUNT; i++) at += sprintf(at, " $v%d", i);
  strcpy(at, ")");
  tight.stack_bytes = 16u << 20;
  mt_clear();
  CHECK(mt_limit(m, tight));
  parsed = mt_parse(source);
  CHECK(parsed && mt_len(parsed) == COUNT + 1);
  for (i = 0; parsed && i < COUNT; i++)
  { const mt_atom *v = mt_at(parsed, (size_t)i + 1);
    snprintf(want, sizeof want, "v%d", i);
    named += mt_kind_of(v) == MT_VARIABLE && strcmp(mt_name(v), want) == 0;
  }
  CHECK(named == COUNT);
  CHECK(mt_limit(m, old));
  mt_drop(parsed);
  free(source);
}

/* The Python seat's solve(): relational let answered as bindings, with the
   template derived from the variables so no third let argument is written. */
static void test_solve_runs_let_backwards_and_reads_bindings_by_name(metta *m)
{ size_t rows = 0, factors = 0;
  mt_space *kb;

  CASE("mt_solve runs + backwards and reads the unknown by its name");
  mt_clear();
  mt_rows (row, mt_solve(m, mt_num(5), E("+", V("p"), 2)))
  { rows++;
    CHECK(mt_int(mt_bound(row, "p")) == 3);
  }
  CHECK(rows == 1 && mt_ok());

  CASE("two unknowns answer every factor pair of 25, each read by name");
  rows = 0;
  mt_rows (row, mt_solve(m, mt_num(25), E("*", V("x"), V("y"))))
  { rows++;
    factors += mt_int(mt_bound(row, "x")) * mt_int(mt_bound(row, "y")) == 25;
  }
  CHECK(rows == 6 && factors == 6 && mt_ok());

  CASE("the template is the pattern's variables, then the subject's new ones");
  rows = 0;
  mt_rows (row, mt_solve(m, E("pair", V("a"), 1), E("pair", 2, V("b"))))
  { rows++;
    CHECK(mt_len(row->atom) == 2 && mt_int(mt_at(row->atom, 0)) == 2 &&
          mt_int(mt_at(row->atom, 1)) == 1);
    CHECK(mt_int(mt_bound(row, "a")) == 2 && mt_int(mt_bound(row, "b")) == 1);
  }
  CHECK(rows == 1 && mt_ok());

  CASE("a named space solves against its own equations");
  kb = mt_space_open(m, "&cmetta-solve");
  CHECK(kb && mt_add(kb, E("=", E("double", V("n")), E("*", 2, V("n")))));
  rows = 0;
  mt_rows (row, mt_solve(kb, mt_num(10), E("double", V("x"))))
  { rows++;
    CHECK(mt_int(mt_bound(row, "x")) == 5);
  }
  CHECK(rows == 1 && mt_ok());
  CHECK(mt_wipe(kb));
  mt_space_close(kb);

  CASE("a solve with no named variable is refused, and _ names none");
  mt_clear();
  CHECK(mt_solve(m, mt_num(5), E("+", 3, 2)) == NULL && mt_error() == MT_MISUSE);
  mt_clear();
  CHECK(mt_solve(m, mt_num(5), E("+", V("_"), 2)) == NULL && mt_error() == MT_MISUSE);
  mt_clear();
}

static void test_a_refusal_carries_the_engines_remedy_and_ground(metta *m)
{ CASE("a refusal says what to do about it, in the engine's own words");
  mt_clear();
  CHECK(mt_run(m, "!(assertEqual 1 2)") == NULL);
  CHECK(mt_error() == MT_ERROR);
  /* The (refusal assertion ...) row's remedy, with <operation> filled from
     this very ball. The same sentence reaches the Python and JavaScript
     seats, because one row renders it once
     [source: engine/spaces/catalog.pl, metta_refusal_declaration/4]. */
  CHECK(mt_remedy() && strstr(mt_remedy(), "correct the claim") != NULL);
  CHECK(mt_remedy() && strstr(mt_remedy(), "assert") != NULL);
  CHECK(mt_ground() && strstr(mt_ground(), "metta-law: ") != NULL);
  CHECK(mt_ground() && strstr(mt_ground(), "report_failed_assertion") != NULL);

  CASE("clearing the error forgets its advice with it");
  mt_clear();
  CHECK(mt_remedy() == NULL);
  CHECK(mt_ground() == NULL);

  CASE("a broken contract of this library's own carries no engine remedy");
  /* mt_bigint("nope") is MISUSE, not a MeTTa refusal: the engine never saw
     it, so there is no row to read and inventing advice here would be this
     seat writing prose the other two seats do not have. */
  mt_drop(mt_bigint("nope"));
  CHECK(mt_error() == MT_MISUSE);
  CHECK(mt_errmsg() != NULL);
  CHECK(mt_remedy() == NULL);
  CHECK(mt_ground() == NULL);
  mt_clear();
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

static void test_prepared_queries_join_and_guard_current_facts(metta *m)
{ mt_space *space = mt_space_open(m, "&c-prepared");
  mt_atom *pattern = E(",", E("Parent", V("x"), V("y")),
                            E("Parent", V("y"), V("z")));
  mt_answers *answers;
  const mt_row *row;
  mt_list all;
  CASE("prepared queries retain shape while joins and guards read current facts");
  mt_clear();
  CHECK(space && pattern);
  CHECK(mt_add(space, E("Parent", "Tom", "Bob")));
  CHECK(mt_add(space, E("Parent", "Bob", "Ann")));
  answers = mt_query(space, mt_keep(pattern), NULL);
  CHECK(answers != NULL);
  row = mt_row_next(answers);
  CHECK(row && mt_bound(row, "x") && mt_bound(row, "z"));
  if ( row )
  { CHECK(strcmp(mt_name(mt_bound(row, "x")), "Tom") == 0);
    CHECK(strcmp(mt_name(mt_bound(row, "z")), "Ann") == 0);
  }
  CHECK(mt_row_next(answers) == NULL && mt_answers_status(answers) == MT_DONE);
  mt_answers_free(answers);
  CHECK(mt_add(space, E("Parent", "Ann", "Zoe")));
  all = mt_all(mt_query(space, mt_keep(pattern), NULL));
  CHECK(all.len == 2 && mt_ok()); mt_list_free(all);
  all = mt_all(mt_query(space, mt_keep(pattern), E("==", V("x"), "Bob")));
  CHECK(all.len == 1 && mt_ok()); mt_list_free(all);
  all = mt_all(mt_query(space, mt_keep(pattern), B(false)));
  CHECK(all.len == 0 && mt_ok()); mt_list_free(all);
  CHECK(mt_space_drop(space)); mt_space_close(space); mt_drop(pattern);
}

typedef struct temporary_query {
  mt_space *space;
  mt_list found;
} temporary_query;

static mt_status ask_with_temporary_facts(metta *m, void *user)
{ temporary_query *work = user;
  (void)m;
  if ( !mt_add(work->space, E("available", "Ada")) ) return mt_error();
  work->found = mt_all(mt_query(work->space, E("available", V("who")), NULL));
  return mt_ok() ? MT_OK : mt_error();
}

static void test_temporary_facts_leave_owned_answers_after_speculation(metta *m)
{ temporary_query work = {mt_space_open(m, "&c-temporary"), {NULL, 0}};
  CASE("speculation discards temporary facts while C retains collected answers");
  mt_clear();
  CHECK(work.space != NULL);
  CHECK(mt_speculate(m, ask_with_temporary_facts, &work) == MT_OK);
  CHECK(work.found.len == 1 && mt_count(work.space) == 0);
  if ( work.found.len )
    CHECK(strcmp(mt_name(mt_at(work.found.items[0], 1)), "Ada") == 0);
  mt_list_free(work.found);
  CHECK(mt_space_drop(work.space)); mt_space_close(work.space);
}

static mt_status write_cell(metta *m, void *user)
{ mt_atom *cell = user;
  mt_atom *answer = mt_one(mt_eval(m, E("change-state!", mt_keep(cell), 99)));
  mt_drop(answer);
  return mt_ok() ? MT_OK : mt_error();
}

static void test_mutable_cells_follow_engine_transactions(metta *m)
{ mt_atom *cell;
  CASE("mutable cells retain engine identity and follow transaction and speculation verdicts");
  mt_clear();
  cell = mt_one(mt_eval(m, E("new-state", 0)));
  CHECK(cell != NULL);
  CHECK(mt_one_int(mt_eval(m, E("get-state", mt_keep(cell)))) == 0);
  CHECK(mt_one_truth(mt_eval(m, E("change-state!", mt_keep(cell), 7))));
  CHECK(mt_one_int(mt_eval(m, E("get-state", mt_keep(cell)))) == 7);
  CHECK(mt_speculate(m, write_cell, cell) == MT_OK);
  CHECK(mt_one_int(mt_eval(m, E("get-state", mt_keep(cell)))) == 7);
  CHECK(mt_transaction(m, write_cell, cell) == MT_OK);
  CHECK(mt_one_int(mt_eval(m, E("get-state", mt_keep(cell)))) == 99);
  mt_drop(cell);
}

static void test_typed_atoms_relate_array_shapes(metta *m)
{ mt_space *space = mt_space_open(m, "&c-shapes");
  mt_atom *shape, *expected;
  CASE("typed atoms express array shapes through shared dimension variables");
  mt_clear();
  CHECK(space != NULL);
  CHECK(mt_do(space, "(: shape-left (Matrix 2 3)) (: shape-right (Matrix 3 4)) "
                    "(: shape-product (-> (Matrix $m $k) (Matrix $k $n) (Matrix $m $n)))"));
  shape = mt_one(mt_eval(space, E("get-type", E("shape-product", "shape-left", "shape-right"))));
  expected = E("Matrix", 2, 4);
  CHECK(mt_eq(shape, expected)); mt_drop(shape); mt_drop(expected);
  CHECK(mt_space_drop(space)); mt_space_close(space);
}

static void test_native_object_types_reach_engine_dispatch(metta *m)
{ CASE("a C object's declared type participates in engine type lookup and dispatch");
  int payload = 7;
  mt_atom *object = mt_object(&payload, "CAccount", NULL);
  mt_atom *answer;
  CHECK(object != NULL);
  answer = mt_one(mt_eval(m, E("get-type", mt_keep(object))));
  CHECK(mt_kind_of(answer) == MT_SYMBOL && strcmp(mt_name(answer), "CAccount") == 0);
  mt_drop(answer);
  CHECK(mt_do(m, "(: c-account-id (-> CAccount CAccount)) (= (c-account-id $x) $x)"));
  answer = mt_one(mt_eval(m, E("c-account-id", mt_keep(object))));
  CHECK(answer && mt_eq(answer, object));
  mt_drop(answer);
  answer = mt_one(mt_eval(m, E("get-type-space", mt_spaceref("&self"), mt_keep(object))));
  CHECK(mt_kind_of(answer) == MT_SYMBOL && strcmp(mt_name(answer), "CAccount") == 0);
  mt_drop(answer);
  mt_object_free(object);
  CASE("a malformed native type refuses instead of disappearing from inference");
  object = mt_object(&payload, "\xff", NULL);
  mt_clear();
  answer = mt_one(mt_eval(m, E("get-type", mt_keep(object))));
  CHECK(!answer && mt_error() >= MT_ERROR);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "invalid UTF-8"));
  mt_drop(answer);
  mt_object_free(object);
  mt_clear();
}

static void test_composed_spaces_read_live_sources(metta *m)
{ mt_space *a = mt_space_open(m, "&c-front"), *b = mt_space_open(m, "&c-back");
  mt_atom *query = E("match", E("superpose", E(mt_spaceref("&c-front"), mt_spaceref("&c-back"))),
                               E("item", V("x")), V("x"));
  mt_list rows;
  CASE("composed queries read each source and preserve multiplicity without copying facts");
  mt_clear();
  CHECK(a && b && query);
  CHECK(mt_add(a, E("item", 1)) && mt_add(b, E("item", 2)));
  rows = mt_all(mt_eval(m, mt_keep(query)));
  CHECK(rows.len == 2 && mt_ok()); mt_list_free(rows);
  CHECK(mt_add(b, E("item", 2)));
  rows = mt_all(mt_eval(m, mt_keep(query)));
  CHECK(rows.len == 3 && mt_ok()); mt_list_free(rows);
  CHECK(mt_space_drop(a) && mt_space_drop(b));
  mt_space_close(a); mt_space_close(b); mt_drop(query);
}

static void test_algebras_are_scoped_engine_data(metta *m)
{ const char *names[] = {"bool", "tropical", "prob"};
  mt_atom *ones[] = {N(1), N(0), N(1)};
  CASE("algebra selection and answer coefficients belong to the engine and restore after close");
  mt_clear();
  for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++)
  { mt_atom *row = mt_one(mt_eval_under(m, S(names[i]), E("+", 2, 3)));
    CHECK(row && mt_len(row) == 2);
    CHECK(mt_int(mt_at(row, 0)) == 5);
    CHECK(mt_eq(mt_at(row, 1), ones[i]));
    mt_drop(row); mt_drop(ones[i]);
  }
  { mt_atom *decl = mt_first(mt_match(mt_catalog(m),
         E("algebra", "tropical", V("combine"), V("extend"), V("zero"),
           V("one"), V("laws"), V("carrier"), V("requires"), V("scope"))));
    CHECK(decl != NULL); mt_drop(decl);
  }
  { mt_answers *a = mt_eval_under(m, S("tropical"), E("superpose", E(1, 2)));
    CHECK(mt_next(a) != NULL); mt_answers_free(a);
  }
  CHECK(mt_one_int(mt_eval(m, E("+", 2, 3))) == 5);
  { mt_list rows = mt_all(mt_eval_under(m, S("c-unknown-algebra"), E("+", 2, 3)));
    CHECK(!rows.len && mt_error() >= MT_ERROR); mt_list_free(rows); mt_clear();
  }
}

/* max-stack-depth bounds an evaluated goal as it bounds a runnable form. The
   two equations overlap at 0, so the recursive one runs on past it; under a
   depth of 20 the finite branch answers 120 and the exhausted one answers its
   StackOverflow error after it. Without the fuel scope mt_eval answered 120
   and then raised a 1Gb host stack overflow [measured 2026-09-24:
   CMeTTa-Examples ch14-seeing-your-program/01-time_and_pragmas]. The pragma
   is engine-wide, so the case clears it again. */
static void test_a_stack_depth_pragma_bounds_an_evaluated_goal(metta *m)
{ mt_atom *pragma = mt_one(mt_eval(m, E("pragma!", "max-stack-depth", 20)));

  CASE("max-stack-depth bounds mt_eval as it bounds a runnable form");
  CHECK(pragma && mt_kind_of(pragma) == MT_EXPR && mt_len(pragma) == 0);
  mt_drop(pragma);
  CHECK(mt_add(m, E("=", E("c-depth-factorial", 0), 1)));
  CHECK(mt_add(m, E("=", E("c-depth-factorial", V("n")),
                    E("*", V("n"), E("c-depth-factorial", E("-", V("n"), 1))))));
  { mt_list rows = mt_all(mt_eval(m, E("c-depth-factorial", 5)));
    mt_atom *overflow = E("Error", -3, "StackOverflow");
    CHECK(mt_ok() && rows.len == 2);
    CHECK(rows.len == 2 && mt_int(rows.items[0]) == 120 && mt_eq(rows.items[1], overflow));
    mt_drop(overflow); mt_list_free(rows);
  }

  CASE("and mt_eval_under, each answer with its annotation");
  { mt_list rows = mt_all(mt_eval_under(m, S("bool"), E("c-depth-factorial", 5)));
    mt_atom *overflow = E("Error", -3, "StackOverflow");
    CHECK(mt_ok() && rows.len == 2);
    CHECK(rows.len == 2 && mt_len(rows.items[0]) == 2 && mt_int(mt_at(rows.items[0], 0)) == 120 &&
          mt_len(rows.items[1]) == 2 && mt_eq(mt_at(rows.items[1], 0), overflow));
    mt_drop(overflow); mt_list_free(rows);
  }
  pragma = mt_one(mt_eval(m, E("pragma!", "max-stack-depth", "none")));
  CHECK(pragma != NULL);
  mt_drop(pragma);
}

/* The generated vocabulary header is the running engine's own: every entry
   of mt_vocabularies[] is one (vocabulary ...) row in &metta with the same
   words in the same order, and &metta holds no such row the header lacks.
   Read against the engine rather than the generator, so the header and its
   generator cannot drift together. Each word finds its own position, a word
   no member has finds nothing and leaves the member unwritten. */
static void test_the_generated_vocabularies_are_the_engines(metta *m)
{ mt_space *catalog = mt_catalog(m);

  CASE("every generated vocabulary is the engine's own row, in its order");
  for (size_t v = 0; v < MT_VOCABULARY_COUNT(mt_vocabularies); v++)
  { const mt_vocabulary *vocab = &mt_vocabularies[v];
    mt_atom **pattern = calloc(vocab->count + 2, sizeof *pattern);
    mt_atom *row;
    char name[24];

    CHECK(pattern != NULL);
    if ( !pattern )
      continue;
    pattern[0] = S("vocabulary");
    pattern[1] = S(vocab->name);
    for (size_t i = 0; i < vocab->count; i++)
    { snprintf(name, sizeof name, "w%zu", i);
      pattern[i + 2] = V(name);
    }
    row = mt_first(mt_match(catalog, mt_exprv(vocab->count + 2, pattern)));
    free(pattern);
    CHECK(row != NULL);
    for (size_t i = 0; row && i < vocab->count; i++)
    { const char *word = mt_name(mt_at(row, i + 2));
      size_t at = vocab->count;
      CHECK(word && strcmp(word, vocab->words[i]) == 0);
      CHECK(mt_vocabulary_index(vocab->words, vocab->count, vocab->words[i], &at) && at == i);
    }
    mt_drop(row);
  }
  { mt_list all = mt_all(mt_atoms(catalog));
    size_t rows = 0;
    for (size_t i = 0; i < all.len; i++)
    { const mt_atom *a = all.items[i];
      rows += mt_kind_of(a) == MT_EXPR && mt_len(a) >= 2 &&
              mt_kind_of(mt_at(a, 0)) == MT_SYMBOL &&
              strcmp(mt_name(mt_at(a, 0)), "vocabulary") == 0;
    }
    CHECK(rows == MT_VOCABULARY_COUNT(mt_vocabularies));
    mt_list_free(all);
  }

  CASE("a word finds its member, and any other word finds none");
  { enum mt_effect_class effect = MT_EFFECT_CLASS_ORACLE_IO;
    CHECK(mt_effect_class_of("writesState", &effect) && effect == MT_EFFECT_CLASS_WRITES_STATE);
    effect = MT_EFFECT_CLASS_ORACLE_IO;
    CHECK(!mt_effect_class_of("writes-state", &effect) && effect == MT_EFFECT_CLASS_ORACLE_IO);
    CHECK(!mt_effect_class_of(NULL, &effect) && effect == MT_EFFECT_CLASS_ORACLE_IO);
  }
}

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
  test_an_answer_keeps_variable_identity(m);
  test_alpha_equivalence_is_a_renaming(m);
  test_the_standard_order_is_the_engines(m);
  test_a_partial_application_answers_as_its_wire_expression(m);
  test_a_refusal_payload_is_an_expression(m);
  test_many_variables_are_named_without_aborting(m);
  test_solve_runs_let_backwards_and_reads_bindings_by_name(m);
  test_a_refusal_carries_the_engines_remedy_and_ground(m);
  test_a_bound_stops_a_runaway_and_says_so(m);
  test_the_counters_measure_engine_work(m);
  test_verbosity_reaches_the_engines_own_door(m);
  test_reopening_is_the_same_runtime(m);
  test_prepared_queries_join_and_guard_current_facts(m);
  test_temporary_facts_leave_owned_answers_after_speculation(m);
  test_mutable_cells_follow_engine_transactions(m);
  test_typed_atoms_relate_array_shapes(m);
  test_native_object_types_reach_engine_dispatch(m);
  test_composed_spaces_read_live_sources(m);
  test_algebras_are_scoped_engine_data(m);
  test_a_stack_depth_pragma_bounds_an_evaluated_goal(m);
  test_the_generated_vocabularies_are_the_engines(m);
#ifdef MT_HAS_AUTO
  test_scope_cleanup_releases_on_every_exit(m);
#endif

  printf("%d checks, %d failures\n", checks, failures);
  mt_close(m);
  return failures == 0 ? 0 : 1;
}
