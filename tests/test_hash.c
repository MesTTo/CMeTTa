/* Purpose: prove that mt_hash covers every atom kind with mt_eq-compatible
 *   structural semantics and an iterative deep-term walk.
 * Assumes: the fault library can construct MT_HANDLE, which has no public C
 *   constructor because it represents a value native to the embedded engine.
 * Guarantees: exits nonzero unless equal atoms of every kind hash alike,
 *   signed zero and expression structure affect the hash, NaN payloads do
 *   not, counted NUL bytes survive, object identity crosses the engine, and a
 *   50,000-level expression hashes without consuming the C call stack.
 * Owns resources: drops every atom, removes its object fixture and closes the
 *   runtime.
 */

#define MT_SHORTHAND
#include <cmetta.h>

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern mt_atom *mt_test_handle_atom(const char *text);

static int failures;

static void expect(bool condition, const char *claim)
{ if ( condition ) return;
  failures++;
  fprintf(stderr, "hash regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
}

static void expect_equal_hash(mt_atom *left, mt_atom *right, const char *kind)
{ expect(left && right && mt_eq(left, right), kind);
  expect(left && right && mt_hash(left) == mt_hash(right),
         "mt_eq atoms must have the same hash");
  mt_drop(left);
  mt_drop(right);
}

static double float_bits(uint64_t bits)
{ double value;
  static_assert(sizeof(value) == sizeof(bits),
                "the NaN-payload regression needs binary64 doubles");
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static void count_release(void *value)
{ (*(int *)value)++;
}

static void test_every_kind(metta *runtime)
{ static const char nul_text[] = { 'a', '\0', 'b' };
  mt_atom *positive_zero = R(0.0);
  mt_atom *negative_zero = R(-0.0);
  mt_atom *nan_a = R(float_bits(UINT64_C(0x7ff8000000000001)));
  mt_atom *nan_b = R(float_bits(UINT64_C(0x7ff8000000000011)));
  mt_atom *symbol = S("same-bytes");
  mt_atom *text = T("same-bytes");
  mt_atom *ordered = E("pair", 1, 2);
  mt_atom *reversed = E("pair", 2, 1);
  mt_atom *object, *retained, *crossed;
  int releases = 0;

  expect_equal_hash(S("alpha"), S("alpha"), "symbols must compare equal");
  expect_equal_hash(T("alpha"), T("alpha"), "text must compare equal");
  expect_equal_hash(mt_textn(nul_text, sizeof(nul_text)),
                    mt_textn(nul_text, sizeof(nul_text)),
                    "counted text with NUL must compare equal");
  expect_equal_hash(N(INT64_MIN), N(INT64_MIN), "integers must compare equal");
  expect_equal_hash(R(3.25), R(3.25), "finite floats must compare equal");
  expect_equal_hash(mt_bigint("9223372036854775808"),
                    mt_bigint("9223372036854775808"),
                    "big integers must compare equal");
  expect_equal_hash(mt_rational(-2, 3), mt_rational(-2, 3),
                    "rationals must compare equal");
  expect_equal_hash(B(true), B(true), "booleans must compare equal");
  expect_equal_hash(V("x"), V("x"), "variables must compare equal");
  expect_equal_hash(mt_unit(), mt_unit(), "unit expressions must compare equal");
  expect_equal_hash(E("f", 1, E("g", 2)), E("f", 1, E("g", 2)),
                    "nested expressions must compare equal");
  expect_equal_hash(mt_spaceref("&hash-space"), mt_spaceref("&hash-space"),
                    "space references must compare equal");
  expect_equal_hash(mt_test_handle_atom("native-handle"),
                    mt_test_handle_atom("native-handle"),
                    "native handles must compare equal");

  expect(!mt_eq(positive_zero, negative_zero),
         "signed zeros must remain different atoms");
  expect(mt_hash(positive_zero) != mt_hash(negative_zero),
         "signed zero must affect the hash");
  expect(mt_eq(nan_a, nan_b), "all NaN payloads must compare equal");
  expect(mt_hash(nan_a) == mt_hash(nan_b),
         "all NaN payloads must hash alike");
  expect(mt_hash(symbol) != mt_hash(text),
         "the kind must domain-separate identical bytes");
  expect(mt_hash(ordered) != mt_hash(reversed),
         "expression child order must affect the hash");
  mt_drop(positive_zero);
  mt_drop(negative_zero);
  mt_drop(nan_a);
  mt_drop(nan_b);
  mt_drop(symbol);
  mt_drop(text);
  mt_drop(ordered);
  mt_drop(reversed);

  object = mt_object(&releases, "hash-object", count_release);
  retained = mt_keep(object);
  expect(mt_add(runtime, object), "the object hash fixture must cross");
  crossed = mt_first(mt_match(runtime, mt_keep(retained)));
  expect(crossed && mt_kind_of(crossed) == MT_OBJECT,
         "the object fixture must return as an object");
  expect(crossed && mt_eq(retained, crossed),
         "the crossed object must retain box identity");
  expect(crossed && mt_hash(retained) == mt_hash(crossed),
         "object identity must hash alike across distinct C atoms");
  expect(mt_del(runtime, mt_keep(retained)),
         "the object hash fixture must be removed");
  expect(mt_object_free(retained), "the object blob must release explicitly");
  mt_drop(crossed);
  expect(releases == 1, "the object fixture must release exactly once");
}

static void test_deep_hash(void)
{ enum { DEPTH = 50000 };
  mt_atom *left = N(0);
  mt_atom *right = N(0);
  int i;

  for (i = 0; i < DEPTH && left && right; i++)
  { left = E("nest", left);
    right = E("nest", right);
  }
  expect(left && right, "the deep hash fixtures must build");
  expect(left && right && mt_hash(left) == mt_hash(right),
         "the deep equal terms must hash alike without recursion");
  expect(left && right && mt_eq(left, right),
         "the deep hash fixtures must remain structurally equal");
  mt_drop(left);
  mt_drop(right);
}

int main(void)
{ metta *runtime = mt_open(NULL);
  expect(runtime != NULL, "the runtime must boot");
  if ( !runtime ) return 1;
  mt_clear();
  expect(mt_hash(NULL) == 0 && mt_error() == MT_MISUSE,
         "a NULL hash input must refuse through the error channel");
  mt_clear();
  test_every_kind(runtime);
  test_deep_hash();
  mt_close(runtime);
  if ( !failures ) puts("structural hash contracts ok");
  return failures ? 1 : 0;
}
