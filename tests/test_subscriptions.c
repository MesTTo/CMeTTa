/* Purpose: exercise committed event delivery and explicit C subscription life.
 * Guarantees: repeated variables filter both edges; rollback delivers nothing,
 * cancellation can roll back, callback refusal preserves committed writes, and
 * self-cancellation releases once [tested: test_subscriptions; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 * Owns resources: all allocations are counted through runtime shutdown.
 */
#include <cmetta.h>
/* The checks below are assert()s, so they stay live whatever NDEBUG a build sets. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "allocation_tracker.h"
#define CASE(name) do { fprintf(stderr, "CASE %s\n", (name)); } while (0)

typedef struct observation {
  metta *runtime;
  unsigned added, removed, released;
  bool cancel, fail;
} observation;

static mt_status notify(void *user, bool added, const mt_atom *atom)
{ observation *o = user;
  assert(mt_len(atom) == 3 && mt_eq(mt_at(atom, 1), mt_at(atom, 2)));
  if ( added ) o->added++; else o->removed++;
  if ( o->cancel )
  { assert(mt_unsubscribe(o->runtime, "changes"));
    assert(!o->released);
  }
  return o->fail ? mt_error_set(MT_ERROR, "notification refused") : MT_OK;
}

static void release(void *user) { ((observation *)user)->released++; }
static mt_subscription subscription(observation *o)
{ return (mt_subscription){"&c-events", mt_expr("f", mt_var("x"), mt_var("x")),
                          notify, o, release}; }

typedef struct writes { mt_space *space; observation *observer; mt_status verdict; } writes;
static mt_status write_scope(metta *runtime, void *data)
{ writes *w = data;
  unsigned before = w->observer->added;
  (void)runtime;
  assert(mt_add(w->space, mt_expr("f", 2, 2)));
  assert(mt_add(w->space, mt_expr("f", 2, 2)));
  assert(w->observer->added == before);
  return w->verdict;
}
static mt_status cancel_scope(metta *runtime, void *data)
{ (void)data; assert(mt_unsubscribe(runtime, "changes")); return MT_FAIL; }
static mt_status register_scope(metta *runtime, void *data)
{ assert(mt_subscribe(runtime, "discarded", subscription(data))); return MT_FAIL; }

static int test_subscriptions(void)
{ CASE("standing queries observe committed changes and retain callback ownership"); tracker allocation = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &allocation});
  metta *m = mt_open(NULL);
  mt_space *space;
  observation o = {.runtime=m}, refused = {.runtime=m};
  writes work;
  size_t before;
  assert(m);
  space = mt_space_open(m, "&c-events"); assert(space);
  assert(mt_subscribe(m, "changes", subscription(&o)));
  assert(!mt_subscribe(m, "changes", subscription(&refused)));
  assert(refused.released == 1 && mt_error() == MT_MISUSE); mt_clear();
  assert(mt_add(space, mt_expr("f", 1, 2)) && !o.added);
  assert(mt_add(space, mt_expr("f", 1, 1)) && o.added == 1);
  assert(mt_del(space, mt_expr("f", 1, 1)) && o.removed == 1);
  work = (writes){space, &o, MT_FAIL};
  assert(mt_transaction(m, write_scope, &work) == MT_FAIL && o.added == 1);
  work.verdict = MT_OK;
  assert(mt_speculate(m, write_scope, &work) == MT_OK && o.added == 1);
  assert(mt_transaction(m, write_scope, &work) == MT_OK && o.added == 3);
  assert(mt_transaction(m, cancel_scope, NULL) == MT_FAIL && !o.released);
  assert(mt_transaction(m, register_scope, &refused) == MT_FAIL && refused.released == 2);
  assert(mt_seam_count(m, "subscription") == 1);
  assert(mt_add(space, mt_expr("f", 3, 3)) && o.added == 4);
  o.fail = true; before = mt_count(space);
  assert(mt_transaction(m, write_scope, &work) == MT_ERROR);
  assert(strstr(mt_errmsg(), "notification refused")); mt_clear();
  assert(mt_count(space) == before + 2 && mt_seam_count(m, "subscription") == 1);
  o.fail = false; o.cancel = true;
  assert(mt_add(space, mt_expr("f", 4, 4)) && o.released == 1);
  assert(!mt_unsubscribe(m, "changes") && mt_ok());
  before = o.added;
  assert(mt_add(space, mt_expr("f", 5, 5)) && o.added == before);
  assert(mt_subscribe(m, "changes", subscription(&refused)));
  assert(mt_add(space, mt_expr("f", 6, 6)) && refused.added == 1);
  assert(mt_unsubscribe(m, "changes") && refused.released == 3);
  mt_space_close(space); mt_close(m);
  assert(!allocation.blocks && !allocation.bytes);
  mt_allocator_set(previous);
  puts("subscriptions: matching, both edges, commit buffering, rollback, failure and reentrant cancellation passed");
  return 0;
}

int main(void)
{ return test_subscriptions();
}
