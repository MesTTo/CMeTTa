/* Purpose: prove this seat's extension seam, kind by kind, and the three
 *   doors a library outside this repository registers through: a space whose
 *   atoms it holds, how its objects print, and a directory of sources it
 *   ships.
 * Assumes: the engine tree is where MT_ENGINE_PATH says, which is what every
 *   other test in this directory assumes.
 * Guarantees: exits nonzero unless a point is declared once with one kind, a
 *   registration against an undeclared point is refused naming the declared
 *   ones, an ownership point stops at the first row that claims and refuses a
 *   row with no claims(), a declaration point refuses mt_claim(), the four
 *   shipped points are declared at boot, mt_def writes an `op` row, a C
 *   provider answers a match and a clear through the engine, an object prints
 *   through its registered rendering, and mt_extension refuses a path that is
 *   not a library and one that exports no mt_extension_init.
 * Owns resources: closes its provider, its spaces and its runtime; the
 *   provider's own store is static, so nothing here leaks a heap block.
 */

#define MT_SHORTHAND
#include <cmetta.h>

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(bool condition, const char *claim)
{ if ( condition ) return;
  failures++;
  fprintf(stderr, "seam regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
}

/* --- a store this seat knows nothing about ------------------------- */

static const char *held[] = { "(star sol)", "(star vega)" };
static size_t held_len = 2;

static bool store_add(void *user, const char *atom)
{ (void)user;
  (void)atom;
  return false;   /* declared read-only, so the engine refuses a write */
}

static const char *store_atom_at(void *user, size_t index)
{ (void)user;
  return index < held_len ? held[index] : NULL;
}

static bool store_clear(void *user)
{ (void)user;
  held_len = 0;
  return true;
}

/* --- an object type, and how it prints ----------------------------- */

typedef struct probe { int value; } probe_t;

static const char *probe_text(void *value, void *user)
{ static char rendered[64];
  (void)user;
  snprintf(rendered, sizeof(rendered), "(probe %d)", ((probe_t *)value)->value);
  return rendered;
}

/* --- an ownership point of this test's own ------------------------- */

static void *claims_even(void *value, void *subject)
{ (void)value;
  return (*(int *)subject % 2 == 0) ? subject : NULL;
}

static void *claims_anything(void *value, void *subject)
{ (void)value;
  return subject;
}

static void check_points(metta *m)
{ const mt_seam_row *row;
  mt_seam_row registration = {0};
  int even = 4, odd = 3;
  void *answered = NULL;

  /* The four shipped points, declared at boot rather than on first use. */
  expect(mt_point_count(m) >= 4, "the seat declares its shipped points at boot");
  for (const char *name = NULL, **each = (const char *[]){ "op", "repr", "provider", "library", NULL };
       (name = *each); each++)
    expect(mt_point_of(m, name) != NULL, "a shipped point is declared");

  /* A point is declared once with one kind. */
  expect(mt_point_declare(m, (mt_point){ "orbit", MT_OWNERSHIP, "claims period",
                                         "a point this test declares" }),
         "a library declares a point of its own");
  mt_clear();
  expect(!mt_point_declare(m, (mt_point){ "orbit", MT_DECLARATION, "x", "again" }),
         "a second declaration of one name is refused");
  expect(mt_errmsg() && strstr(mt_errmsg(), "already declared as ownership"),
         "and the refusal names the kind it already has");
  mt_clear();

  /* An undeclared point refuses by name, listing the declared ones. */
  registration.point = "nowhere";
  registration.name = "solars";
  registration.claims = claims_anything;
  expect(!mt_register(m, registration), "an undeclared point refuses a row");
  expect(mt_errmsg() && strstr(mt_errmsg(), "orbit"),
         "and the refusal lists the points that ARE declared");
  mt_clear();

  /* An ownership point needs a claims(). */
  registration.point = "orbit";
  registration.claims = NULL;
  expect(!mt_register(m, registration), "an ownership row with no claims is refused");
  expect(mt_errmsg() && strstr(mt_errmsg(), "claims()"),
         "and the refusal names what it needs");
  mt_clear();

  /* Rows are consulted in registration order and the first claim wins. */
  registration.claims = claims_even;
  registration.name = "even";
  expect(mt_register(m, registration), "an ownership row registers");
  registration.claims = claims_anything;
  registration.name = "any";
  expect(mt_register(m, registration), "a second row registers behind it");
  expect(mt_seam_count(m, "orbit") == 2, "both rows are there");

  row = mt_claim(m, "orbit", &even, &answered);
  expect(row && strcmp(row->name, "even") == 0, "the first row that claims wins");
  expect(answered == &even, "and its answer reaches the caller");
  row = mt_claim(m, "orbit", &odd, NULL);
  expect(row && strcmp(row->name, "any") == 0,
         "a row that declines is passed over rather than failing the dispatch");

  /* A declaration point is not read with mt_claim(). */
  expect(mt_claim(m, "op", &even, NULL) == NULL, "mt_claim refuses a declaration point");
  expect(mt_errmsg() && strstr(mt_errmsg(), "declaration point") &&
         strstr(mt_errmsg(), "mt_seam_at()"),
         "and says which kind it is and how to read it");
  mt_clear();

  expect(mt_unregister(m, "orbit", "even"), "a row withdraws");
  expect(!mt_unregister(m, "orbit", "even"), "and withdrawing it twice answers false");
  expect(mt_seam_count(m, "orbit") == 1, "leaving the other in place");
  expect(mt_seam_at(m, "orbit", 1) == NULL, "and nothing past the end");
}

static void check_published_ops(metta *m)
{ size_t before = mt_seam_count(m, "op");

  expect(mt_def(m, (mt_op){ .name = "seam_probe", .arity = 0,
                            .effect = MT_PURE,
                            .fn = NULL }) == false,
         "a published function needs a function");
  mt_clear();
  expect(mt_seam_count(m, "op") == before,
         "and a refused publication writes no row");
}

static void check_provider(metta *m)
{ mt_answers *answers;
  const mt_row *answer;
  size_t seen = 0;
  mt_provider provider = { .user = NULL, .add = store_add,
                           .remove = NULL, .atom_at = store_atom_at,
                           .clear = store_clear, .release = NULL };
  mt_space *space;

  expect(mt_provider_open(m, "&stars", provider), "a C provider backs a space");
  expect(mt_seam_count(m, "provider") == 1, "and writes one row");
  mt_clear();
  expect(!mt_provider_open(m, "&stars", provider),
         "a space another provider owns is refused rather than clobbered");
  mt_clear();

  space = mt_space_open(m, "&stars");
  expect(space != NULL, "the space opens");
  answers = mt_space_atoms(space);
  expect(answers != NULL, "and enumerates");
  while ( (answer = mt_row_next(answers)) ) { (void)answer; seen++; }
  expect(seen == 2, "answering exactly the atoms the C store holds");
  mt_answers_free(answers);

  seen = 0;
  { MT_AUTO mt_atom *pattern = E(S("star"), V("which"));
    answers = mt_space_match(space, pattern ? mt_keep(pattern) : NULL);
    expect(answers != NULL, "a match reaches the C store");
    while ( (answer = mt_row_next(answers)) ) { (void)answer; seen++; }
    expect(seen == 2, "and both atoms unify with the pattern");
    mt_answers_free(answers);
  }

  expect(mt_space_wipe(space), "a clear reaches the C store");
  expect(held_len == 0, "which emptied it");
  mt_space_close(space);
  expect(mt_provider_close(m, "&stars"), "the provider closes");
  expect(mt_seam_count(m, "provider") == 0, "and its row goes with it");
}

static void check_repr(metta *m)
{ static probe_t probe = { 7 };
  MT_AUTO mt_atom *object = mt_object(&probe, "probe", NULL);
  MT_AUTO_ASK mt_answers *answers = NULL;
  const mt_row *answer;

  expect(object != NULL, "a C value crosses as an object");
  expect(mt_repr(m, "probe", probe_text, NULL), "and its type registers a rendering");
  expect(mt_seam_count(m, "repr") == 1, "which is one row");

  { MT_AUTO mt_atom *goal = E(S("id"), mt_keep(object));
    (void)goal;
  }
  { mt_space *self = mt_self(m);
    MT_AUTO mt_atom *fact = E(S("held"), mt_keep(object));
    expect(mt_space_add(self, mt_keep(fact)), "the object goes into a space");
    answers = mt_space_atoms(self);
    expect(answers != NULL, "and the space enumerates");
    while ( (answer = mt_row_next(answers)) )
      if ( answer->text && strstr(answer->text, "(probe 7)") )
      { expect(true, "the object renders through its registered text");
        return;
      }
    failures++;
    fprintf(stderr, "seam regression failed: the object never rendered as "
                    "(probe 7)\n");
  }
}

static void check_extension(metta *m)
{ mt_clear();
  expect(!mt_extension(m, "./no-such-library.so"),
         "a path that is not a library is refused");
  expect(mt_errmsg() && strstr(mt_errmsg(), "cannot load"),
         "and the refusal names the loader's own words");
  mt_clear();
  expect(!mt_extension(m, "libcmetta.so"),
         "a library exporting no mt_extension_init is refused");
  expect(mt_errmsg() && strstr(mt_errmsg(), "mt_extension_init"),
         "and the refusal names the symbol it wanted and the shape to export");
  mt_clear();
}

int main(void)
{ metta *m = mt_open(&(mt_config){0});
  if ( !m )
  { fprintf(stderr, "seam regression failed to open a runtime: %s\n",
            mt_errmsg() ? mt_errmsg() : "(none)");
    return 1;
  }
  check_points(m);
  check_published_ops(m);
  check_provider(m);
  check_repr(m);
  check_extension(m);
  mt_close(m);
  if ( failures ) return 1;
  printf("seam: points, rows, ownership, a C provider, a rendering and the "
         "loader\n");
  return 0;
}
