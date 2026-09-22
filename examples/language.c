/* Purpose: exercise C notation for queries, temporary facts and engine state.
 * Owns resources: closes the subscription, drops terms and lists, releases the
 * named space and closes the runtime on success or failure.
 * Guarantees: the reported joins, event count, cell value and annotation are
 * checked against engine answers [tested: make test; commit=1a60e2a3cce69d5d6bda67100186939d707f4397].
 */
#include <cmetta.h>
#include <stdio.h>

typedef struct work {
  mt_space *space;
  const mt_atom *pattern;
  mt_list found;
} work;

static mt_status add_link(metta *runtime, void *user)
{ work *w = user;
  (void)runtime;
  if ( !mt_add(w->space, mt_expr("Parent", "Ann", "Zoe")) ) return mt_error();
  w->found = mt_all(mt_query(w->space, mt_keep(w->pattern), NULL));
  return mt_ok() ? MT_OK : mt_error();
}

static mt_status changed(void *user, bool added, const mt_atom *atom)
{ size_t *events = user;
  (void)atom;
  if ( added ) ++*events;
  return MT_OK;
}

int main(void)
{ metta *m = mt_open(NULL);
  mt_space *kb = NULL;
  mt_atom *pattern = NULL, *cell = NULL, *annotated = NULL;
  mt_list rows = {0};
  size_t events = 0;
  bool subscribed = false;
  int result = 1;
  work w = {0};
  if ( !m ) goto done;
  kb = mt_space_open(m, "&c-language");
  if ( !kb ) goto done;
  pattern = mt_expr(",", mt_expr("Parent", mt_var("x"), mt_var("y")),
                          mt_expr("Parent", mt_var("y"), mt_var("z")));
  if ( !pattern || !mt_add(kb, mt_expr("Parent", "Tom", "Bob")) ||
       !mt_add(kb, mt_expr("Parent", "Bob", "Ann")) ) goto done;
  rows = mt_all(mt_query(kb, mt_keep(pattern), NULL));
  if ( !mt_ok() || rows.len != 1 ) goto done;
  printf("prepared query: %zu answer\n", rows.len);
  subscribed = mt_subscribe(m, "parent-changes", (mt_subscription){
      .space = "&c-language", .pattern = mt_expr("Parent", mt_var("a"), mt_var("b")),
      .notify = changed, .user = &events});
  if ( !subscribed ) goto done;
  w = (work){kb, pattern, {0}};
  if ( mt_speculate(m, add_link, &w) != MT_OK || w.found.len != 2 ||
       mt_count(kb) != 2 || events != 0 ) goto done;
  printf("temporary fact: %zu answers, %zu committed events\n", w.found.len, events);
  mt_list_free(w.found); w.found = (mt_list){0};
  if ( mt_transaction(m, add_link, &w) != MT_OK || w.found.len != 2 ||
       mt_count(kb) != 3 || events != 1 ) goto done;
  printf("committed fact: %zu answers, %zu event\n", w.found.len, events);
  cell = mt_one(mt_eval(m, mt_expr("new-state", 0)));
  if ( !cell || !mt_one_truth(mt_eval(m, mt_expr("change-state!", mt_keep(cell), 7))) ||
       mt_one_int(mt_eval(m, mt_expr("get-state", mt_keep(cell)))) != 7 ) goto done;
  annotated = mt_one(mt_eval_under(m, mt_sym("tropical"), mt_expr("+", 2, 3)));
  if ( !annotated || mt_int(mt_at(annotated, 0)) != 5 ||
       mt_int(mt_at(annotated, 1)) != 0 || !mt_ok() ) goto done;
  printf("cell: 7; tropical answer: %s\n", mt_show(annotated));
  result = 0;
done:
  if ( result ) fprintf(stderr, "language example: %s\n",
                        mt_errmsg() ? mt_errmsg() : "unexpected answer");
  if ( subscribed && !mt_unsubscribe(m, "parent-changes") ) result = 1;
  mt_list_free(rows); mt_list_free(w.found);
  mt_drop(pattern); mt_drop(cell); mt_drop(annotated);
  if ( kb && !mt_space_drop(kb) ) result = 1;
  mt_space_close(kb); mt_close(m);
  return result;
}
