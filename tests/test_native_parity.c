/* Purpose: compare native atom construction and unification with the live engine.
 * Guarantees: 64 repeated-variable combinations agree with engine unification;
 * integers and borrowed atoms survive transport, forms never execute, and a
 * single-occurrence removal refuses an unbound atom
 * [tested: test_native_atoms_match_engine_terms,
 * test_unicode_terms_and_names; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 * Owns resources: releases every atom, list, substitution and runtime.
 * Guarantees: source plans preserve all five C callback effects, join overloads,
 *   and do not execute the goal [tested: test_source_effect_plans; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5].
 * Guarantees: mt_bigrational reduces a ratio of any width to the value the
 *   engine's rdiv gives, across the int64 boundaries and 200 seeded ratios
 *   whose halves share a random factor, refuses every other spelling, and the
 *   result crosses into the engine and back whole
 *   [tested: test_wide_ratios_agree_with_the_engine; commit=WORKTREE].
 * Open Obligations: None.
 */
#include <cmetta.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "allocation_tracker.h"
#define CASE(name) do { fprintf(stderr, "CASE %s\n", (name)); } while (0)

static void round_trip(metta *m, mt_atom *atom)
{ mt_atom *returned = mt_one(mt_eval(m, mt_expr("quote", mt_keep(atom))));
  assert(returned && mt_eq(atom, returned) && mt_hash(atom) == mt_hash(returned));
  mt_drop(returned); mt_drop(atom);
}

/* A seeded xorshift64* stream, so a failing draw reproduces
   [source: S. Vigna, "An experimental exploration of Marsaglia's xorshift
   generators, scrambled", ACM TOMS 42(4), 2016]. */
static uint64_t g_stream = 0x9e3779b97f4a7c15u;

static unsigned draw(unsigned n)
{ g_stream ^= g_stream >> 12;
  g_stream ^= g_stream << 25;
  g_stream ^= g_stream >> 27;
  return (unsigned)((g_stream * 2685821657736338717u) >> 33) % n;
}

/* An integer of 1 to 60 decimal digits, negative when asked. */
static mt_atom *random_integer(int negative)
{ char text[64];
  unsigned n = draw(60) + 1, i = 0;
  if ( negative ) text[i++] = '-';
  text[i++] = (char)('1' + draw(9));
  while ( i < n + (unsigned)negative ) text[i++] = (char)('0' + draw(10));
  text[i] = '\0';
  return mt_bigint(text);
}

/* The digits of an Int or BigInt, into `buf` for an Int. */
static const char *digits_of(const mt_atom *a, char *buf, size_t size)
{ if ( mt_kind_of(a) != MT_INT ) return mt_name(a);
  snprintf(buf, size, "%lld", (long long)mt_int(a));
  return buf;
}

/* "N/D" through mt_bigrational against the engine's (math-rational N D),
   then through the engine and back. Both halves are TAKEN. */
static void agrees_with_rdiv(metta *m, const char *text, mt_atom *numerator,
                             mt_atom *denominator)
{ mt_atom *engine = mt_one(mt_eval(m, mt_expr("math-rational", numerator, denominator)));
  mt_atom *mine = mt_bigrational(text);
  if ( !engine || !mine || !mt_eq(engine, mine) )
    fprintf(stderr, "%s: engine %s, mt_bigrational %s\n", text,
            engine ? mt_show(engine) : "nothing", mine ? mt_show(mine) : mt_errmsg());
  assert(engine && mine && mt_eq(engine, mine) && mt_hash(engine) == mt_hash(mine));
  mt_drop(engine);
  round_trip(m, mine);
}

static void test_wide_ratios_agree_with_the_engine(metta *m)
{ static const char *const boundary[] = {
    "9223372036854775807/1", "9223372036854775808/1", "-9223372036854775808/1",
    "-9223372036854775809/1", "9223372036854775807/9223372036854775806",
    "9223372036854775808/9223372036854775807",
    "-9223372036854775808/9223372036854775807", "18446744073709551616/2",
    "6/4", "-6/4", "0/5", "-0/3", "0007/0014",
    "1/1606938044258990275541962092341162602522202993782792835301376" };
  static const char *const malformed[] = {
    "", "/", "1/", "/2", "1//2", "+1/2", "1/-2", "1.5/2", " 1/2", "1/2 ",
    "--1/2", "-/2", "12", "1/0", "-0/00" };
  /* Two halves of 120 digits each, the products of 60-digit draws, a sign,
     a slash and the terminator. */
  char text[2 * 120 + 3], left[24], right[24];
  size_t i;
  unsigned round;

  CASE("mt_bigrational reduces a ratio of any width as the engine's rdiv does");
  assert(mt_one_truth(mt_eval(m, mt_expr("import!", "&self", mt_expr("library", "lib_math")))));
  for (i = 0; i < sizeof boundary / sizeof *boundary; i++)
  { const char *slash = strchr(boundary[i], '/');
    snprintf(left, sizeof left, "%.*s", (int)(slash - boundary[i]), boundary[i]);
    agrees_with_rdiv(m, boundary[i], mt_bigint(left), mt_bigint(slash + 1));
  }
  for (round = 0; round < 200; round++)
  { mt_atom *a = random_integer((int)draw(2)), *b = random_integer(0), *g = random_integer(0);
    mt_atom *numerator = mt_one(mt_eval(m, mt_expr("*", a, mt_keep(g))));
    mt_atom *denominator = mt_one(mt_eval(m, mt_expr("*", b, g)));
    assert(numerator && denominator);
    snprintf(text, sizeof text, "%s/%s", digits_of(numerator, left, sizeof left),
             digits_of(denominator, right, sizeof right));
    agrees_with_rdiv(m, text, numerator, denominator);
  }

  CASE("every other spelling is refused as a misuse");
  for (i = 0; i < sizeof malformed / sizeof *malformed; i++)
  { mt_clear();
    assert(!mt_bigrational(malformed[i]) && mt_error() == MT_MISUSE);
  }
  mt_clear();

  CASE("a BigRational reads as its canonical text and nothing narrower");
  { mt_atom *wide = mt_bigrational("-2/3213876088517980551083924184682325205044405987565585670602752");
    assert(mt_kind_of(wide) == MT_BIGRATIONAL &&
           strcmp(mt_name(wide), "-1/1606938044258990275541962092341162602522202993782792835301376") == 0);
    assert(mt_float(wide) == 0.0 && mt_error() == MT_UNSUPPORTED);
    mt_clear();
    assert(mt_ratio_of(wide).den == 0 && mt_error() == MT_MISUSE);
    mt_clear();
    mt_drop(wide);
  }
}

static mt_status identity(mt_call *call, void *user)
{ (void)user; return mt_answer(call, mt_keep(mt_arg(call, 0))); }

static const char *render_text(void *value, void *user)
{ (void)user; return value; }

static void test_source_effect_plans(metta *m)
{
  const mt_effect classes[] = {MT_PURE, MT_LOOKUP, MT_NONDET, MT_WRITES, MT_IO};
  const char *names[] = {"plan-pure", "plan-read", "plan-many", "plan-write", "plan-io"};
  for (size_t i = 0; i < sizeof(classes)/sizeof(*classes); ++i)
  {
    assert(mt_def(m, (mt_op){.name=names[i], .arity=1, .effect=classes[i], .fn=identity}));
    mt_atom *plan = mt_effect_plan(m, mt_expr(names[i], 7));
    assert(plan && strcmp(mt_name(mt_at(plan, 0)), "EffectPlan") == 0);
    if (strcmp(mt_name(mt_at(plan, 1)), mt_effect_str(classes[i])) != 0)
      fprintf(stderr, "effect plan for %s: %s\n", names[i], mt_show(plan));
    assert(strcmp(mt_name(mt_at(plan, 1)), mt_effect_str(classes[i])) == 0);
    assert(mt_len(mt_at(plan, 2)) == 1);
    mt_drop(plan);
  }
  size_t count = mt_count(m);
  mt_atom *plan = mt_effect_plan(m, mt_parse("(add-atom &self (not-executed 7))"));
  assert(plan && strcmp(mt_name(mt_at(plan, 1)), "writesState") == 0);
  assert(mt_count(m) == count); mt_drop(plan);
  plan = mt_effect_plan(m, mt_parse("(plan-pure (plan-io 7))"));
  assert(plan && strcmp(mt_name(mt_at(plan, 1)), "oracleIO") == 0);
  assert(mt_len(mt_at(plan, 2)) == 2); mt_drop(plan);
  assert(mt_def(m, (mt_op){.name="plan-pure", .arity=2, .effect=MT_WRITES, .fn=identity}));
  plan = mt_effect_plan(m, mt_expr("plan-pure", 7));
  assert(plan && strcmp(mt_name(mt_at(plan, 1)), "writesState") == 0); mt_drop(plan);
  for (size_t i = 0; i < sizeof(classes)/sizeof(*classes); ++i) assert(mt_undef(m, names[i]));
  mt_list rows = mt_all(mt_match(mt_catalog(m), mt_parse("(effect plan-pure $class)")));
  assert(mt_ok() && !rows.len); mt_list_free(rows);
}

static void test_unicode_terms_and_names(metta *m)
{ CASE("UTF-8 terms and names preserve their bytes and reject malformed input"); const char borrowed[] = "λ 😀";
  mt_atom *atom = mt_parse("(λ $日本語 \"λ 😀\")");
  mt_list forms;
  mt_space *space;
  assert(atom && strcmp(mt_name(mt_at(atom, 0)), "λ") == 0);
  assert(strcmp(mt_name(mt_at(atom, 1)), "日本語") == 0);
  char *shown = mt_show_dup(atom);
  assert(shown && strcmp(shown, "(λ $日本語 \"λ 😀\")") == 0);
  mt_free(shown); mt_drop(atom);
  round_trip(m, mt_sym("日本語"));
  round_trip(m, mt_text_ref(borrowed, sizeof(borrowed) - 1, NULL, NULL));
  forms = mt_forms("λ \"😀\"");
  assert(forms.len == 2 && strcmp(mt_name(forms.items[0]), "λ") == 0);
  assert(strcmp(mt_name(forms.items[1]), "😀") == 0); mt_list_free(forms);
  assert(mt_def(m, (mt_op){.name="λ-echo", .arity=1, .effect=MT_PURE, .fn=identity}));
  atom = mt_one(mt_run(m, "!(λ-echo \"λ 😀\")"));
  assert(atom && strcmp(mt_name(atom), borrowed) == 0); mt_drop(atom);
  assert(mt_undef(m, "λ-echo"));
  space = mt_space_open(m, "&日本語"); assert(space);
  assert(mt_do(space, "(= (λ) \"😀\")"));
  atom = mt_one(mt_run(space, "!(λ)"));
  assert(atom && strcmp(mt_name(atom), "😀") == 0); mt_drop(atom);
  forms = mt_all(mt_load(space, "tests/fixtures/λ.metta"));
  assert(mt_ok() && forms.len == 1 && strcmp(mt_name(forms.items[0]), borrowed) == 0);
  mt_list_free(forms);
  assert(mt_space_drop(space)); mt_space_close(space);
  const char *invalid[] = {"\xff", "\x80", "\xc0\x80", "\xc1\xbf",
    "\xe0\x80\x80", "\xed\xa0\x80", "\xf0\x80\x80\x80",
    "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xc2", "\xe1\xbf",
    "\xf0\x90\x80", "\xe2x"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
  { assert(!mt_parse(invalid[i]) && mt_error() == MT_MISUSE);
    assert(strstr(mt_errmsg(), "invalid UTF-8")); mt_clear();
    atom = mt_sym(invalid[i]); shown = mt_show_dup(atom);
    assert(!shown && mt_error() == MT_MISUSE); mt_drop(atom); mt_clear();
  }
  const char *valid[] = {"\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80",
    "\xed\x9f\xbf", "\xee\x80\x80", "\xef\xbf\xbf",
    "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"};
  for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); i++)
    round_trip(m, mt_text(valid[i]));
  assert(mt_repr(m, "unicode-object", render_text, NULL));
  atom = mt_object((void *)borrowed, "unicode-object", NULL);
  shown = mt_show_dup(atom);
  assert(shown && strcmp(shown, borrowed) == 0); mt_free(shown); mt_drop(atom);
  atom = mt_object((void *)"\xff", "unicode-object", NULL);
  shown = mt_show_dup(atom);
  assert(!shown && mt_error() >= MT_ERROR && strstr(mt_errmsg(), "invalid UTF-8"));
  mt_drop(atom); mt_clear();
  atom = mt_one(mt_run(m, "!(quote \"λ 😀\")"));
  assert(atom && strcmp(mt_name(atom), borrowed) == 0); mt_drop(atom);
}

static void test_native_atoms_match_engine_terms(metta *m)
{ CASE("native atoms preserve identity through the engine");
  mt_list forms;
  size_t before;
  const char bytes[] = "a\0b";
  round_trip(m, mt_unum(UINT64_MAX));
  round_trip(m, mt_bigint("00000018446744073709551615"));
  round_trip(m, mt_bigint("-00000018446744073709551615"));
  round_trip(m, mt_bigint("-000"));
  round_trip(m, mt_bigint("9223372036854775807"));
  round_trip(m, mt_bigint("-9223372036854775808"));
  round_trip(m, mt_text_ref(bytes, 3, NULL, NULL));
  { mt_atom *child = mt_unum(UINT64_MAX);
    const mt_atom *children[] = {child, child};
    round_trip(m, mt_expr_ref(2, children, NULL, NULL));
    mt_drop(child);
  }
  for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++)
  { mt_atom *pattern = mt_expr("f", mt_var("x"), mt_var("x"));
    mt_atom *candidate = mt_expr("f", i, j);
    mt_bindings *bindings = mt_unify(pattern, candidate);
    mt_list answers = mt_all(mt_eval(m, mt_expr("unify", mt_keep(pattern),
                                mt_keep(candidate), mt_keep(pattern), "Empty")));
    assert(mt_ok() && answers.len == (bindings ? 1u : 0u));
    if ( bindings )
    { mt_atom *substituted = mt_substitute(pattern, bindings);
      assert(mt_eq(substituted, answers.items[0])); mt_drop(substituted);
    }
    mt_bindings_free(bindings); mt_list_free(answers);
    mt_drop(pattern); mt_drop(candidate);
  }
  before = mt_count(m);
  forms = mt_forms("(= (never-compiled $x) $x) !(add-atom &self (never-stored 7))");
  assert(forms.len == 2 && mt_count(m) == before);
  assert(strcmp(mt_name(mt_at(mt_at(forms.items[0], 1), 1)), "x") == 0);
  mt_list_free(forms);
  forms = mt_forms("; empty\n"); assert(!forms.len && mt_ok()); mt_list_free(forms);
  forms = mt_forms("(valid) (unfinished");
  assert(!forms.len && !forms.items && mt_error() >= MT_ERROR); mt_clear();
  assert(mt_add(m, mt_expr("duplicate", 1)) && mt_add(m, mt_expr("duplicate", 1)));
  assert(mt_del(m, mt_expr("duplicate", 1)) && mt_count(m) == before + 1);
  assert(!mt_del(m, mt_var("x")) && mt_error() >= MT_ERROR);
  mt_clear(); assert(mt_count(m) == before + 1);
  assert(mt_del(m, mt_expr("duplicate", 1)) && mt_count(m) == before);
  puts("native parity: UTF-8, 64 unification pairs, canonical integers, borrowed transport, unevaluated forms and occurrence removal passed");
}

int main(void)
{ tracker allocation = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &allocation});
  metta *m = mt_open(NULL);
  assert(m);
  test_unicode_terms_and_names(m);
  test_native_atoms_match_engine_terms(m);
  test_source_effect_plans(m);
  test_wide_ratios_agree_with_the_engine(m);
  mt_close(m);
  assert(!allocation.blocks && !allocation.bytes);
  mt_allocator_set(previous);
  return 0;
}
