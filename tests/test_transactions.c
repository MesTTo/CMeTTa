/* Purpose: verify C callback scopes against the engine's commit, rollback,
 * speculation, held-cursor and space lifecycle services.
 * Owns resources: scopes close before cursors, spaces and runtime.
 * Guarantees: source clauses and C registrations share the transaction verdict
 * [tested: test_transactions,
 * test_engine_scopes_cannot_abandon_c_registrations; commit=1a60e2a3cce69d5d6bda67100186939d707f4397].
 */
#include <cmetta.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "allocation_tracker.h"
#define CASE(name) do { fprintf(stderr, "CASE %s\n", (name)); } while (0)

typedef struct work {
  mt_space *space;
  mt_status verdict;
  int value;
  bool nested;
  mt_answers *escaped;
} work;

static void check(bool condition, const char *claim, int line)
{ if ( condition ) return;
  fprintf(stderr, "transaction line %d: %s; %s\n", line, claim,
          mt_errmsg() ? mt_errmsg() : "no error");
  assert(condition);
}
#define CHECK(x) check((x), #x, __LINE__)

static mt_atom *local_call(void)
{ return mt_exprv(1, (mt_atom *[]){mt_sym("local-value")}); }

static mt_status change(metta *m, void *user)
{ work *w = user;
  char source[64];
  mt_answers *write;
  mt_atom *found;
  snprintf(source, sizeof(source), "(= (local-value) %d)", w->value);
  CHECK(mt_do(w->space, source));
  CHECK(mt_add(w->space, mt_sym("direct-write")));
  write = mt_eval(w->space, mt_expr("add-atom",
                  mt_spaceref(mt_space_name(w->space)), mt_expr("cursor-write", 1)));
  CHECK(write);
  mt_answers_free(write);
  found = mt_first(mt_match(w->space, mt_expr("cursor-write", 1)));
  CHECK(found);
  mt_drop(found);
  w->escaped = mt_eval(w->space, mt_expr("superpose", mt_expr(9, 10)));
  CHECK(w->escaped);
  if ( w->nested )
  { work inner = {w->space, MT_OK, 99, false, NULL};
    CHECK(mt_transaction(m, change, &inner) == MT_OK);
    mt_answers_free(inner.escaped);
  }
  if ( w->verdict >= MT_ERROR ) return mt_error_set(w->verdict, "rollback reason from C");
  return w->verdict;
}

static mt_status value(mt_call *call, void *user)
{ return mt_answer(call, mt_num(*(int *)user)); }

static unsigned old_releases, new_releases;
static void release_old(void *unused) { (void)unused; old_releases++; }
static void release_new(void *unused) { (void)unused; new_releases++; }
static int old_value = 1, new_value = 2;

static mt_status replace(metta *m, void *unused)
{ (void)unused;
  CHECK(mt_def(m, (mt_op){"tx-op", 0, MT_PURE, value, &new_value}));
  CHECK(mt_register(m, (mt_seam_row){.point="tx-record", .name="entry",
                     .value=&new_value, .release=release_new}));
  CHECK(mt_one_int(mt_eval(m, mt_exprv(1, (mt_atom *[]){mt_sym("tx-op")}))) == 2);
  CHECK(!old_releases);
  return MT_FAIL;
}

static mt_status unregister_then_fail(metta *m, void *unused)
{ (void)unused;
  CHECK(mt_undef(m, "tx-op"));
  CHECK(mt_unregister(m, "tx-record", "entry"));
  return MT_FAIL;
}

static mt_status fail_to_close(metta *m, void *unused)
{ (void)unused;
  mt_close(m);
  return mt_error();
}

static unsigned raw_calls;
static mt_status raw_registration(mt_call *call, void *unused)
{ (void)unused;
  raw_calls++;
  if ( !mt_register(mt_of(call), (mt_seam_row){.point="tx-record", .name="raw"}) )
    return mt_fail(call, mt_errmsg());
  return mt_answer(call, mt_unit());
}

static mt_status raw_scope(metta *m, void *unused)
{ mt_list rows;
  (void)unused;
  rows = mt_all(mt_run(m, "!(transaction (raw-registration))"));
  mt_list_free(rows);
  return mt_ok() ? MT_OK : mt_error();
}

static void test_engine_scopes_cannot_abandon_c_registrations(void)
{ tracker allocation = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &allocation});
  metta *m = mt_open(NULL);
  size_t before;
  CASE("every scope enclosing C registry mutation must retain its own rollback owner");
  CHECK(m);
  mt_clear();
  CHECK(mt_point_declare(m, (mt_point){"tx-record", MT_DECLARATION, "value", "owned test value"}));
  before = mt_seam_count(m, "tx-record");
  CHECK(mt_def(m, (mt_op){"raw-registration", 0, MT_WRITES, raw_registration, NULL}));
  mt_status status = raw_scope(m, NULL);
  CHECK(raw_calls == 1);
  CHECK(status >= MT_ERROR);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "mt_transaction")); mt_clear();
  CHECK(mt_seam_count(m, "tx-record") == before);
  CHECK(mt_transaction(m, raw_scope, NULL) >= MT_ERROR);
  CHECK(raw_calls == 2);
  CHECK(mt_errmsg() && strstr(mt_errmsg(), "mt_transaction")); mt_clear();
  CHECK(mt_seam_count(m, "tx-record") == before);
  CHECK(mt_undef(m, "raw-registration"));
  mt_close(m);
  CHECK(!allocation.blocks && !allocation.bytes);
  mt_allocator_set(previous);
}

static int test_transactions(void)
{ CASE("transactions and speculation restore engine state and registrations"); tracker allocation = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &allocation});
  metta *m = mt_open(NULL);
  mt_space *a, *b, *anonymous;
  mt_atom *name, *found;
  mt_list loaded;
  work w;
  size_t count;
  CHECK(m);
  a = mt_space_open(m, "&c-transaction-a");
  b = mt_space_open(m, "&c-transaction-b");
  CHECK(a && b);
  CHECK(mt_do(b, "(= (local-value) 17)"));
  w = (work){a, MT_OK, 41, false, NULL};
  CHECK(mt_transaction(m, change, &w) == MT_OK);
  CHECK(mt_one_int(mt_eval(a, local_call())) == 41);
  CHECK(mt_one_int(mt_eval(b, local_call())) == 17);
  CHECK(mt_int(mt_next(w.escaped)) == 9);
  mt_answers_free(w.escaped);
  count = mt_count(a);
  w.verdict = MT_FAIL; w.value = 42;
  CHECK(mt_transaction(m, change, &w) == MT_FAIL);
  CHECK(mt_count(a) == count && mt_one_int(mt_eval(a, local_call())) == 41);
  CHECK(!mt_next(w.escaped) && mt_answers_status(w.escaped) == MT_DONE);
  mt_answers_free(w.escaped);
  w.verdict = MT_NOMEM;
  CHECK(mt_transaction(m, change, &w) == MT_NOMEM);
  CHECK(strcmp(mt_errmsg(), "rollback reason from C") == 0);
  mt_answers_free(w.escaped);
  mt_clear();
  w.verdict = MT_FAIL; w.nested = true;
  CHECK(mt_transaction(m, change, &w) == MT_FAIL);
  mt_answers_free(w.escaped);
  CHECK(mt_count(a) == count && mt_one_int(mt_eval(a, local_call())) == 41);
  w.verdict = MT_OK; w.nested = false;
  CHECK(mt_speculate(m, change, &w) == MT_OK);
  mt_answers_free(w.escaped);
  CHECK(mt_count(a) == count && mt_one_int(mt_eval(a, local_call())) == 41);
  CHECK(mt_point_declare(m, (mt_point){"tx-record", MT_DECLARATION, "value", "owned test value"}));
  CHECK(mt_register(m, (mt_seam_row){.point="tx-record", .name="entry",
                    .value=&old_value, .release=release_old}));
  CHECK(mt_def(m, (mt_op){"tx-op", 0, MT_PURE, value, &old_value}));
  CHECK(mt_transaction(m, replace, NULL) == MT_FAIL);
  CHECK(new_releases == 1 && !old_releases);
  CHECK(mt_seam_at(m, "tx-record", 0)->value == &old_value);
  CHECK(mt_one_int(mt_eval(m, mt_exprv(1, (mt_atom *[]){mt_sym("tx-op")}))) == 1);
  CHECK(mt_transaction(m, unregister_then_fail, NULL) == MT_FAIL);
  CHECK(!old_releases && mt_seam_count(m, "tx-record") == 1);
  CHECK(mt_one_int(mt_eval(m, mt_exprv(1, (mt_atom *[]){mt_sym("tx-op")}))) == 1);
  CHECK(mt_transaction(m, fail_to_close, NULL) == MT_MISUSE);
  mt_clear();
  name = mt_one(mt_eval(m, mt_exprv(1, (mt_atom *[]){mt_sym("new-space")})));
  CHECK(name && mt_kind_of(name) == MT_SPACE);
  anonymous = mt_space_open(m, mt_name(name));
  mt_drop(name);
  CHECK(anonymous && mt_add(anonymous, mt_num(7)));
  CHECK(mt_space_drop(anonymous));
  mt_space_close(anonymous);
  loaded = mt_all(mt_load(b, "tests/fixtures/load_test.metta"));
  mt_list_free(loaded);
  CHECK(mt_ok());
  CHECK(mt_one_int(mt_eval(b, mt_exprv(1, (mt_atom *[]){mt_sym("cmetta-loaded-value")}))) == 73);
  found = mt_first(mt_match(a, mt_expr("cursor-write", 1)));
  CHECK(found);
  mt_drop(found);
  CHECK(mt_space_drop(a) && mt_space_drop(b));
  mt_space_close(a); mt_space_close(b);
  mt_close(m);
  CHECK(old_releases == 1 && new_releases == 1);
  CHECK(!allocation.blocks && !allocation.bytes);
  mt_allocator_set(previous);
  puts("transactions: isolated source, commit, rollback, nesting, speculation, cursor ownership and registry restoration passed");
  return 0;
}

int main(void)
{ int result = test_transactions();
  test_engine_scopes_cannot_abandon_c_registrations();
  return result;
}
