/* Purpose: prove the pure-C unifier returns a complete normalized
 *   substitution and that mt_substitute applies it without recursion.
 * Assumes: mt_unify and mt_substitute borrow their inputs, while an
 *   mt_bindings object owns the variable and value atoms exposed by accessors.
 * Guarantees: exits nonzero unless ground success and mismatch are distinct;
 *   variables from either side and every variadic operand bind simultaneously;
 *   aliases normalize, `_` stays anonymous, cycles stay finite, substitutions
 *   remain partial, 1,000 generated reconstructions hold, ownership transfers
 *   are exact, and 50,000 levels are data.
 * Owns resources: drops every input and result and frees every substitution.
 */

#define MT_SHORTHAND
#include <cmetta.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GENERATED_CASES = 1000, ALIAS_COUNT = 5000, DEEP_COUNT = 50000 };

static int failures;

static void expect(bool condition, const char *claim)
{ if ( condition ) return;
  failures++;
  fprintf(stderr, "unify regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
}

static void expect_value(const mt_bindings *bindings, const char *name,
                         mt_atom *expected, const char *claim)
{ const mt_atom *actual = mt_binding(bindings, name);
  expect(actual && expected && mt_eq(actual, expected), claim);
  mt_drop(expected);
}

static void drop_atoms(mt_atom **atoms, size_t count)
{ size_t i;
  for (i = 0; i < count; i++) mt_drop(atoms[i]);
  free(atoms);
}

static void test_pair_unification_and_mismatch(void)
{ mt_atom *left = E("f", V("x"), "b");
  mt_atom *right = E("f", "a", V("y"));
  mt_bindings *bindings = mt_unify(left, right);
  const mt_atom *variable;

  expect(bindings != NULL, "variables in both operands must unify");
  expect(mt_bindings_len(bindings) == 2,
         "both operand variables must appear in the substitution");
  variable = mt_binding_var(bindings, 0);
  expect(variable && strcmp(mt_name(variable), "y") == 0,
         "the binding order must match the Python work-list");
  variable = mt_binding_var(bindings, 1);
  expect(variable && strcmp(mt_name(variable), "x") == 0,
         "the second work-list binding must remain iterable");
  mt_drop(left);
  mt_drop(right);
  expect_value(bindings, "x", S("a"),
               "the left variable must retain its ground value");
  expect_value(bindings, "y", S("b"),
               "the right variable must retain its ground value");
  mt_bindings_free(bindings);

  left = E("pair", V("same"), V("same"));
  right = E("pair", "a", "b");
  mt_clear();
  bindings = mt_unify(left, right);
  expect(bindings == NULL, "a repeated variable contradiction must mismatch");
  expect(mt_ok(), "a structural mismatch must not be reported as an error");
  mt_drop(left);
  mt_drop(right);

  left = E("ground", 1, "a");
  right = E("ground", 1, "a");
  bindings = mt_unify(left, right);
  expect(bindings && mt_bindings_len(bindings) == 0,
         "equal ground terms must return a non-NULL empty substitution");
  mt_drop(left);
  mt_drop(right);
  mt_bindings_free(bindings);
}

static void test_variadic_and_anonymous_unification(void)
{ mt_atom *atoms[3];
  mt_bindings *bindings;

  atoms[0] = E("f", V("x"), "b");
  atoms[1] = E("f", "a", V("y"));
  atoms[2] = E("f", V("p"), V("q"));
  bindings = mt_unifyv(3, (const mt_atom *const *)atoms);
  expect(bindings != NULL,
         "all variadic operands must agree through one substitution");
  expect(bindings && mt_bindings_len(bindings) == 4,
         "all four variadic variables must be returned");
  expect_value(bindings, "x", S("a"), "x must normalize to a");
  expect_value(bindings, "y", S("b"), "y must normalize to b");
  expect_value(bindings, "p", S("a"), "p must normalize to a");
  expect_value(bindings, "q", S("b"), "q must normalize to b");
  mt_bindings_free(bindings);
  mt_drop(atoms[0]);
  mt_drop(atoms[1]);
  mt_drop(atoms[2]);

  atoms[0] = E("f", V("x"));
  atoms[1] = E("f", "a");
  atoms[2] = E("f", "b");
  mt_clear();
  bindings = mt_unifyv(3, (const mt_atom *const *)atoms);
  expect(bindings == NULL,
         "a later variadic contradiction must reject the whole substitution");
  expect(mt_ok(), "a variadic mismatch must not poison the error channel");
  mt_drop(atoms[0]);
  mt_drop(atoms[1]);
  mt_drop(atoms[2]);

  atoms[0] = E("pair", V("_"), V("_"));
  atoms[1] = E("pair", "a", "b");
  bindings = mt_unify(atoms[0], atoms[1]);
  expect(bindings && mt_bindings_len(bindings) == 0,
         "each anonymous variable occurrence must bind nothing");
  mt_bindings_free(bindings);
  mt_drop(atoms[0]);
  mt_drop(atoms[1]);
}

static void test_alias_normalization(void)
{ mt_atom **atoms = calloc(ALIAS_COUNT + 1, sizeof(*atoms));
  mt_bindings *bindings = NULL;
  mt_atom *expected;
  size_t i;
  char name[32];

  expect(atoms != NULL, "the alias fixture array must allocate");
  if ( !atoms ) return;
  for (i = 0; i < ALIAS_COUNT; i++)
  { snprintf(name, sizeof(name), "x%zu", i);
    atoms[i] = V(name);
    if ( !atoms[i] ) break;
  }
  if ( i == ALIAS_COUNT ) atoms[i] = S("a");
  if ( i != ALIAS_COUNT || !atoms[i] )
  { expect(false, "every alias fixture atom must build");
    drop_atoms(atoms, i + (atoms[i] != NULL));
    return;
  }

  bindings = mt_unifyv(ALIAS_COUNT + 1,
                       (const mt_atom *const *)atoms);
  expect(bindings && mt_bindings_len(bindings) == ALIAS_COUNT,
         "the complete alias chain must bind every variable");
  drop_atoms(atoms, ALIAS_COUNT + 1);
  if ( !bindings ) return;

  expected = S("a");
  for (i = 0; i < ALIAS_COUNT; i++)
  { const mt_atom *value;
    snprintf(name, sizeof(name), "x%zu", i);
    value = mt_binding(bindings, name);
    if ( !value || !mt_eq(value, expected) )
    { expect(false, "every alias must normalize to the ground terminus");
      break;
    }
  }
  mt_drop(expected);
  mt_bindings_free(bindings);
}

static void test_generated_round_trips(void)
{ int i;
  for (i = 0; i < GENERATED_CASES; i++)
  { char symbol[32];
    mt_atom *pattern, *ground, *rebuilt;
    mt_bindings *bindings;

    snprintf(symbol, sizeof(symbol), "value-%d", (i * 37) % GENERATED_CASES);
    pattern = E("row", V("same"), E("cell", V("value"), i), V("same"));
    ground = E("row", S(symbol), E("cell", (i * 97) - 50000, i),
               S(symbol));
    bindings = mt_unify(pattern, ground);
    rebuilt = bindings ? mt_substitute(pattern, bindings) : NULL;
    if ( !bindings || !rebuilt || !mt_eq(rebuilt, ground) )
    { expect(false,
             "every generated pattern substitution must reconstruct its fact");
      mt_drop(pattern);
      mt_drop(ground);
      mt_drop(rebuilt);
      mt_bindings_free(bindings);
      return;
    }
    mt_drop(rebuilt);
    mt_bindings_free(bindings);

    bindings = mt_unify(ground, pattern);
    rebuilt = bindings ? mt_substitute(pattern, bindings) : NULL;
    if ( !bindings || !rebuilt || !mt_eq(rebuilt, ground) )
    { expect(false,
             "every generated ground-first unification must reconstruct too");
      mt_drop(pattern);
      mt_drop(ground);
      mt_drop(rebuilt);
      mt_bindings_free(bindings);
      return;
    }
    mt_drop(pattern);
    mt_drop(ground);
    mt_drop(rebuilt);
    mt_bindings_free(bindings);
  }
}

static void test_partial_and_cyclic_substitution(void)
{ mt_atom *left = E("p", V("x"));
  mt_atom *right = E("p", "a");
  mt_atom *source = E("q", V("x"), V("free"));
  mt_atom *expected = E("q", "a", V("free"));
  mt_atom *substituted;
  mt_bindings *bindings = mt_unify(left, right);

  expect(bindings != NULL, "the substitution fixture must unify");
  substituted = bindings ? mt_substitute(source, bindings) : NULL;
  expect(substituted && mt_eq(substituted, expected),
         "substitution must replace bound variables and retain unbound ones");
  mt_drop(left);
  mt_drop(right);
  mt_drop(source);
  mt_drop(expected);
  mt_drop(substituted);
  mt_bindings_free(bindings);

  left = V("x");
  right = E("f", V("x"));
  bindings = mt_unify(left, right);
  expect(bindings != NULL, "a no-occurs-check cycle must unify");
  mt_drop(left);
  mt_drop(right);
  expect_value(bindings, "x", E("f", V("x")),
               "cycle normalization must remain finite");
  source = E("g", V("x"));
  expected = E("g", E("f", V("x")));
  substituted = mt_substitute(source, bindings);
  expect(substituted && mt_eq(substituted, expected),
         "substitution must not re-enter a cyclic replacement");
  mt_drop(source);
  mt_drop(expected);
  mt_drop(substituted);
  mt_bindings_free(bindings);
}

static void test_deep_terms_are_data(void)
{ mt_atom *deep = S("leaf");
  mt_atom *variable = V("deep");
  mt_bindings *bindings;
  int i;

  for (i = 0; i < DEEP_COUNT && deep; i++) deep = E("nest", deep);
  expect(deep != NULL, "the deep normalization fixture must build");
  bindings = deep ? mt_unify(variable, deep) : NULL;
  expect(bindings != NULL, "a variable must unify with a deep ground term");
  expect(bindings && mt_binding(bindings, "deep") == deep,
         "normalizing a deep ground value must preserve its identity");
  mt_drop(variable);
  mt_drop(deep);
  mt_bindings_free(bindings);

  { mt_atom *hole = V("x");
    mt_atom *leaf = S("leaf");
    mt_atom *pattern = V("x");
    mt_atom *answer = S("leaf");
    mt_atom *result;

    bindings = mt_unify(hole, leaf);
    mt_drop(hole);
    mt_drop(leaf);
    for (i = 0; i < DEEP_COUNT && pattern && answer; i++)
    { pattern = E("nest", pattern);
      answer = E("nest", answer);
    }
    expect(pattern && answer, "the deep substitution fixtures must build");
    result = pattern && bindings ? mt_substitute(pattern, bindings) : NULL;
    expect(result && answer && mt_eq(result, answer),
           "substitution must walk 50,000 levels without C recursion");
    mt_drop(pattern);
    mt_drop(answer);
    mt_drop(result);
    mt_bindings_free(bindings);
  }
}

static void count_release(void *value)
{ (*(int *)value)++;
}

static void test_binding_ownership_and_contracts(void)
{ int releases = 0;
  mt_atom *variable = V("resource");
  mt_atom *object = mt_object(&releases, "unify-resource", count_release);
  mt_bindings *bindings = mt_unify(variable, object);

  expect(bindings != NULL, "an object value must unify with a variable");
  mt_drop(variable);
  mt_drop(object);
  expect(releases == 0,
         "the substitution must own its object value after inputs are dropped");
  expect(bindings && mt_value(mt_binding(bindings, "resource")) == &releases,
         "the borrowed binding value must retain the original object");
  mt_bindings_free(bindings);
  expect(releases == 1, "freeing the substitution must release its object once");

  variable = S("ground");
  bindings = mt_unify(variable, variable);
  mt_clear();
  expect(bindings && mt_binding(bindings, "absent") == NULL && mt_ok(),
         "an unbound variable name must return NULL without an error");
  mt_clear();
  expect(mt_substitute(NULL, bindings) == NULL && mt_error() == MT_MISUSE,
         "a NULL substitution input must report misuse");
  mt_bindings_free(bindings);
  mt_drop(variable);

  variable = S("one");
  { const mt_atom *one[1] = { variable };
    mt_clear();
    expect(mt_unifyv(1, one) == NULL && mt_error() == MT_MISUSE,
           "variadic unification must require at least two operands");
  }
  mt_drop(variable);
}

int main(void)
{ test_pair_unification_and_mismatch();
  test_variadic_and_anonymous_unification();
  test_alias_normalization();
  test_generated_round_trips();
  test_partial_and_cyclic_substitution();
  test_deep_terms_are_data();
  test_binding_ownership_and_contracts();
  if ( !failures ) puts("unification and substitution contracts ok");
  return failures ? 1 : 0;
}
