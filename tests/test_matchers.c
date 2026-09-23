/* Purpose: prove custom grounded matching, candidate bindings and ownership.
 * Owns resources: deterministically releases matcher blobs and cursor state.
 * Guarantees: matching refuses, binds, streams, propagates errors and closes
 *   on abandonment [tested: make test; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5].
 * Open Obligations: None.
 */
#include "cmetta.h"
/* The checks below are assert()s, so they stay live whatever NDEBUG a build sets. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct state { unsigned calls, releases, closed, next; } state;
static void released(void *user) { ++((state *)user)->releases; }
static mt_status prefix(mt_call *call, void *user)
{
  state *s = user; ++s->calls;
  const mt_atom *other = mt_arg(call, 0);
  const char *text = mt_name(other);
  return text && text[0] == 'a' ? mt_answer(call, mt_keep(other)) : MT_FAIL;
}
static mt_status next(void *user, mt_atom **out)
{
  state *s = user;
  if (s->next == 3) return MT_DONE;
  *out = mt_expr("value", (int64_t)++s->next);
  return *out ? MT_ROW : mt_error();
}
static void closed(void *user) { ++((state *)user)->closed; }
static mt_status enumerate(mt_call *call, void *user)
{ return mt_answer_iter(call, (mt_iterator){user, next, closed}); }
static mt_status broken(mt_call *call, void *user)
{ (void)user; return mt_fail(call, "matcher refusal"); }
int main(void)
{
  state s = {0};
  assert(!mt_matcher(NULL, &s, released) && s.releases == 1);
  assert(mt_error() == MT_MISUSE); mt_clear(); s.releases = 0;
  metta *m = mt_open(NULL); assert(m);
  mt_atom *matcher = mt_matcher(prefix, &s, released); assert(matcher);
  assert(mt_one_truth(mt_eval(m, mt_expr("unify", mt_keep(matcher), "abbey", mt_bool(true), mt_bool(false)))));
  assert(!mt_one_truth(mt_eval(m, mt_expr("unify", mt_keep(matcher), "zebra", mt_bool(true), mt_bool(false)))) && mt_ok());
  assert(s.calls == 2);
  assert(mt_one_truth(mt_eval(m, mt_expr("unify", mt_keep(matcher), mt_var("whole"), mt_bool(true), mt_bool(false)))));
  assert(s.calls == 2);
  assert(mt_object_free(matcher) && s.releases == 1);

  state stream = {0}; matcher = mt_matcher(enumerate, &stream, released);
  mt_answers *rows = mt_eval(m, mt_expr("unify", mt_keep(matcher),
      mt_expr("value", mt_var("x")), mt_var("x"), "Empty"));
  assert(rows && mt_int(mt_next(rows)) == 1);
  mt_answers_free(rows); assert(stream.closed == 1 && stream.next == 1);
  assert(mt_object_free(matcher) && stream.releases == 1);
  stream = (state){0}; matcher = mt_matcher(enumerate, &stream, released);
  mt_list all = mt_all(mt_eval(m, mt_expr("unify", mt_keep(matcher),
      mt_expr("value", mt_var("x")), mt_var("x"), "Empty")));
  assert(mt_ok() && all.len == 3 && mt_int(all.items[2]) == 3);
  mt_list_free(all); assert(stream.closed == 1);
  assert(mt_object_free(matcher) && stream.releases == 1);

  matcher = mt_matcher(broken, NULL, NULL);
  all = mt_all(mt_eval(m, mt_expr("unify", mt_keep(matcher), "x", mt_bool(true), mt_bool(false))));
  assert(!all.len && mt_error() >= MT_ERROR && strstr(mt_errmsg(), "matcher refusal"));
  mt_list_free(all); mt_clear(); assert(mt_object_free(matcher));
  mt_close(m); assert(mt_ok());
  puts("matchers: refusal, bindings, streams, errors and ownership passed");
  return 0;
}
