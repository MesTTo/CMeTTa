/* Purpose: exercise typed C providers through the engine's foreign-space seam.
 * Guarantees: candidates are unified, errors are preserved, missing capabilities
 * refuse, and captured participants survive replacement
 * [tested: test_providers; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 * Owns resources: each store owns retained atoms and a stack of transaction
 * snapshots; each query owns a snapshot until the engine closes its iterator.
 * Guarantees: completed captures release the final provider owner immediately
 *   [tested: test_providers; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5].
 * Guarantees: a compound stored through a provider comes back as the term it
 *   was, applying, sharing its variables and removing by value
 *   [tested: test_a_stored_compound_comes_back_whole; commit=256a3a6aa4248e89c7b007edacc6832d62eb4594].
 * Guarantees: a provider that promises rules holds equations the engine
 *   compiles, and one that does not refuses them
 *   [tested: test_a_rules_provider_holds_a_program; commit=4cae9c81468cb9fc4a5831d0ec3b5905db276f04].
 * Open Obligations: None.
 */
#include <cmetta.h>
/* The checks below are assert()s, so they stay live whatever NDEBUG a build sets. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "allocation_tracker.h"
#define CASE(name) do { fprintf(stderr, "CASE %s\n", (name)); } while (0)

typedef struct counters { unsigned opened, closed, begins, commits, rollbacks, releases; } counters;
typedef struct snapshot { mt_list atoms; struct snapshot *previous; } snapshot;
typedef struct store {
  mt_list atoms;
  snapshot *saved;
  counters *counts;
  bool fail;
} store;
typedef struct cursor { store *owner; mt_list atoms; const mt_atom *pattern; size_t at; } cursor;

static void check(bool condition, const char *claim, int line)
{ if ( condition ) return;
  fprintf(stderr, "provider line %d: %s; %s\n", line, claim,
          mt_errmsg() ? mt_errmsg() : "no error");
  assert(condition);
}
#define CHECK(x) check((x), #x, __LINE__)

static mt_list copy(mt_list atoms)
{ mt_list result = {0};
  if ( !atoms.len ) return result;
  result.items = mt_calloc(atoms.len, sizeof(*result.items));
  CHECK(result.items);
  for (; result.len < atoms.len; result.len++)
    result.items[result.len] = mt_keep(atoms.items[result.len]);
  return result;
}

static mt_status add(void *user, const mt_atom *atom)
{ store *s = user;
  mt_atom **items = mt_resize(s->atoms.items, (s->atoms.len + 1) * sizeof(*items));
  if ( !items ) return MT_NOMEM;
  s->atoms.items = items;
  s->atoms.items[s->atoms.len++] = mt_keep(atom);
  return MT_OK;
}

static mt_status remove_atom(void *user, const mt_atom *atom, bool *removed)
{ store *s = user;
  *removed = false;
  for (size_t i = 0; i < s->atoms.len; i++)
    if ( mt_eq(atom, s->atoms.items[i]) )
    { mt_drop(s->atoms.items[i]);
      memmove(s->atoms.items + i, s->atoms.items + i + 1,
              (--s->atoms.len - i) * sizeof(*s->atoms.items));
      *removed = true;
      break;
    }
  return MT_OK;
}

static mt_status clear(void *user)
{ store *s = user;
  mt_list_free(s->atoms); s->atoms = (mt_list){0};
  return MT_OK;
}

static mt_status next(void *user, mt_atom **answer)
{ cursor *c = user;
  CHECK(c->pattern);
  if ( c->owner->fail && c->at == 1 )
    return mt_error_set(MT_ERROR, "provider stopped after one candidate");
  if ( c->at == c->atoms.len ) return MT_DONE;
  *answer = mt_keep(c->atoms.items[c->at++]);
  return MT_ROW;
}

static void close_cursor(void *user)
{ cursor *c = user;
  c->owner->counts->closed++;
  mt_list_free(c->atoms); mt_free(c);
}

static mt_status match(void *user, const mt_atom *pattern, size_t limit, mt_iterator *answers)
{ store *s = user;
  cursor *c = mt_alloc(sizeof(*c));
  (void)limit; /* Over-approximation must not truncate before engine unification. */
  if ( !c ) return MT_NOMEM;
  *c = (cursor){s, copy(s->atoms), pattern, 0};
  s->counts->opened++;
  *answers = (mt_iterator){c, next, close_cursor};
  return MT_OK;
}

static mt_status begin(void *user)
{ store *s = user;
  snapshot *saved = mt_alloc(sizeof(*saved));
  if ( !saved ) return MT_NOMEM;
  *saved = (snapshot){copy(s->atoms), s->saved};
  s->saved = saved; s->counts->begins++;
  return MT_OK;
}

static mt_status commit(void *user)
{ store *s = user;
  snapshot *saved = s->saved;
  CHECK(saved);
  s->saved = saved->previous; s->counts->commits++;
  mt_list_free(saved->atoms); mt_free(saved);
  return MT_OK;
}

static mt_status rollback(void *user)
{ store *s = user;
  snapshot *saved = s->saved;
  CHECK(saved);
  mt_list_free(s->atoms); s->atoms = saved->atoms;
  s->saved = saved->previous; s->counts->rollbacks++;
  mt_free(saved);
  return MT_OK;
}

static void release(void *user)
{ store *s = user;
  CHECK(!s->saved && s->counts->opened == s->counts->closed);
  s->counts->releases++;
  mt_list_free(s->atoms); mt_free(s);
}

static mt_provider provider(counters *counts, bool transactional)
{ store *s = mt_calloc(1, sizeof(*s));
  CHECK(s); s->counts = counts;
  return (mt_provider){.user=s, .add=add, .remove=remove_atom, .match=match,
                       .clear=clear, .begin=transactional ? begin : NULL,
                       .commit=transactional ? commit : NULL,
                       .rollback=transactional ? rollback : NULL, .release=release};
}

typedef struct scope { mt_space *space; mt_status verdict; } scope;
static mt_status write_scope(metta *m, void *data)
{ scope *s = data;
  (void)m;
  CHECK(mt_add(s->space, mt_expr("stored", 9)));
  CHECK(mt_count(s->space) == 1);
  return s->verdict;
}

typedef struct replacement { mt_space *space; counters *new_counts; mt_status verdict; } replacement;
static mt_status replace_scope(metta *m, void *data)
{ replacement *r = data;
  CHECK(mt_add(r->space, mt_expr("old", 1)));
  CHECK(mt_provider_close(m, mt_space_name(r->space)));
  CHECK(mt_provider_open(m, mt_space_name(r->space), provider(r->new_counts, true)));
  CHECK(mt_add(r->space, mt_expr("new", 2)));
  return r->verdict;
}

static int test_providers(void)
{ CASE("foreign spaces preserve typed rows and captured transaction ownership"); tracker allocation = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &allocation});
  metta *m = mt_open(NULL);
  mt_space *space;
  mt_answers *answers;
  mt_atom *atom;
  mt_list list;
  counters counts = {0}, readonly_counts = {0}, old_counts = {0}, new_counts = {0};
  mt_provider p;
  store *s;
  scope work;
  replacement change;
  int payload = 37;
  CHECK(m);
  p = provider(&counts, true); s = p.user;
  CHECK(mt_provider_open(m, "&c-provider", p));
  space = mt_space_open(m, "&c-provider"); CHECK(space);
  CHECK(mt_add(space, mt_expr("edge", 1, 2)));
  CHECK(mt_add(space, mt_expr("edge", 3, 3)));
  CHECK(mt_add(space, mt_expr("edge", 3, 3)));
  list = mt_all(mt_match(space, mt_expr("edge", mt_var("x"), mt_var("x"))));
  CHECK(list.len == 2); mt_list_free(list);
  CHECK(counts.opened == counts.closed);
  s->fail = true;
  list = mt_all(mt_atoms(space));
  CHECK(!list.len && !list.items && strstr(mt_errmsg(), "provider stopped after one candidate"));
  s->fail = false; mt_clear();
  CHECK(mt_wipe(space));
  CHECK(mt_add(space, mt_expr("object", mt_object(&payload, "payload", NULL))));
  atom = mt_one(mt_atoms(space));
  CHECK(atom && mt_value(mt_at(atom, 1)) == &payload); mt_drop(atom);
  CHECK(mt_wipe(space));
  work = (scope){space, MT_FAIL};
  CHECK(mt_transaction(m, write_scope, &work) == MT_FAIL);
  CHECK(mt_count(space) == 0 && counts.rollbacks == 1);
  work.verdict = MT_OK;
  CHECK(mt_speculate(m, write_scope, &work) == MT_OK);
  CHECK(mt_count(space) == 0 && counts.rollbacks == 2);
  CHECK(mt_transaction(m, write_scope, &work) == MT_OK);
  CHECK(mt_count(space) == 1 && counts.commits == 1);
  answers = mt_atoms(space); CHECK(mt_next(answers));
  CHECK(mt_provider_close(m, "&c-provider"));
  CHECK(!counts.releases);
  mt_answers_free(answers);
  CHECK(counts.releases == 1);
  mt_space_close(space);

  p = provider(&readonly_counts, false); p.add = NULL;
  CHECK(mt_provider_open(m, "&c-readonly", p));
  space = mt_space_open(m, "&c-readonly"); CHECK(space);
  CHECK(!mt_add(space, mt_expr("forbidden", 1)) && mt_error() >= MT_ERROR);
  CHECK(strstr(mt_errmsg(), "add")); mt_clear();
  CHECK(mt_provider_close(m, "&c-readonly")); mt_space_close(space);

  CHECK(mt_provider_open(m, "&c-replaced", provider(&old_counts, true)));
  space = mt_space_open(m, "&c-replaced"); CHECK(space);
  change = (replacement){space, &new_counts, MT_OK};
  CHECK(mt_transaction(m, replace_scope, &change) == MT_OK);
  CHECK(old_counts.commits == 1 && new_counts.commits == 1);
  CHECK(mt_provider_close(m, "&c-replaced")); mt_space_close(space);
  mt_close(m);
  CHECK(counts.releases == 1 && readonly_counts.releases == 1);
  CHECK(old_counts.releases == 1 && new_counts.releases == 1);
  CHECK(!allocation.blocks && !allocation.bytes);
  mt_allocator_set(previous);
  puts("providers: typed objects, candidate unification, multiplicity, late errors, capability refusal, transactions and retained replacement passed");
  return 0;
}

/* A value the engine stores through a provider comes back as the value it
   stored, as the native space gives it back: a partial application still
   applies, a variable it shares with the atom around it is still shared,
   removing it by value removes it, and the list that spells its expression
   is another atom, which removes nothing. */
static int test_a_stored_compound_comes_back_whole(void)
{ CASE("a compound stored through a provider comes back as the term it was"); tracker allocation = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &allocation});
  metta *m = mt_open(NULL);
  counters counts = {0};
  mt_provider p;
  store *s;
  CHECK(m);
  p = provider(&counts, false); s = p.user;
  CHECK(mt_provider_open(m, "&c-held", p));
  CHECK(mt_one_truth(mt_eval(m, mt_expr("let", mt_var("p"), mt_expr("id", mt_expr("+", 1)),
      mt_expr("add-atom", mt_spaceref("&c-held"), mt_expr("stored", mt_var("p")))))));
  CHECK(s->atoms.len == 1 && mt_kind_of(mt_at(s->atoms.items[0], 1)) == MT_HANDLE);
  CHECK(mt_one_int(mt_eval(m, mt_expr("match", mt_spaceref("&c-held"),
      mt_expr("stored", mt_var("x")), mt_expr(mt_var("x"), 2)))) == 3);
  /* Each removal is read to its end: a cursor freed unread never runs. */
  mt_list_free(mt_all(mt_eval(m, mt_expr("remove-atom", mt_spaceref("&c-held"),
      mt_expr("stored", mt_expr("partial", "+", mt_expr(1)))))));
  CHECK(mt_ok() && s->atoms.len == 1);
  mt_list_free(mt_all(mt_eval(m, mt_expr("let", mt_var("p"), mt_expr("id", mt_expr("+", 1)),
      mt_expr("remove-atom", mt_spaceref("&c-held"), mt_expr("stored", mt_var("p")))))));
  CHECK(mt_ok() && s->atoms.len == 0);
  CHECK(mt_one_truth(mt_eval(m, mt_expr("let", mt_var("p"), mt_expr("id", mt_expr("+", mt_var("v"))),
      mt_expr("add-atom", mt_spaceref("&c-held"), mt_expr("pair", mt_var("v"), mt_var("p")))))));
  CHECK(mt_one_int(mt_eval(m, mt_expr("match", mt_spaceref("&c-held"),
      mt_expr("pair", mt_var("a"), mt_var("b")),
      mt_expr("let", mt_var("a"), 5, mt_expr(mt_var("b"), 1))))) == 6);
  CHECK(mt_provider_close(m, "&c-held"));
  mt_close(m);
  CHECK(counts.releases == 1 && counts.opened == counts.closed);
  CHECK(!allocation.blocks && !allocation.bytes);
  mt_allocator_set(previous);
  puts("providers: a stored compound applies, shares, removes by value and stays apart from its spelling");
  return 0;
}

/* The rules promise, both ways: an equation added to a space that holds
   rules is stored in C and compiled by the engine, answering in that space;
   a space that makes no such promise refuses the equation rather than
   storing one that could never fire. */
static int test_a_rules_provider_holds_a_program(void)
{ CASE("a provider that promises rules holds a program, and one that does not refuses it"); tracker allocation = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &allocation});
  metta *m = mt_open(NULL);
  counters program_counts = {0}, data_counts = {0};
  mt_provider p;
  store *program_store, *data_store;
  mt_space *program, *data;
  mt_atom *rule;
  CHECK(m);
  p = provider(&program_counts, false); program_store = p.user;
  p.rules = true;
  CHECK(mt_provider_open(m, "&c-program", p));
  p = provider(&data_counts, false); data_store = p.user;
  CHECK(mt_provider_open(m, "&c-data", p));
  CHECK((program = mt_space_open(m, "&c-program")) != NULL);
  CHECK((data = mt_space_open(m, "&c-data")) != NULL);
  rule = mt_expr("=", mt_expr("c-double", mt_var("x")), mt_expr("*", 2, mt_var("x")));

  CHECK(mt_add(program, mt_keep(rule)));
  CHECK(program_store->atoms.len == 1);
  CHECK(mt_one_int(mt_eval(m, mt_expr("metta", mt_expr("c-double", 21), "%Undefined%",
                                      mt_spaceref("&c-program")))) == 42);
  CHECK(mt_one_int(mt_eval(program, mt_expr("c-double", 21))) == 42);

  mt_clear();
  CHECK(!mt_add(data, mt_keep(rule)));
  CHECK(mt_error() == MT_ERROR && mt_errmsg() && strstr(mt_errmsg(), "rules") != NULL);
  CHECK(data_store->atoms.len == 0);
  mt_clear();

  mt_drop(rule);
  mt_space_close(program);
  mt_space_close(data);
  CHECK(mt_provider_close(m, "&c-program") && mt_provider_close(m, "&c-data"));
  mt_close(m);
  CHECK(program_counts.releases == 1 && data_counts.releases == 1);
  CHECK(!allocation.blocks && !allocation.bytes);
  mt_allocator_set(previous);
  puts("providers: a rules provider holds a program the engine compiles, and a data provider refuses one");
  return 0;
}

int main(void)
{ return test_providers() || test_a_stored_compound_comes_back_whole() ||
         test_a_rules_provider_holds_a_program();
}
