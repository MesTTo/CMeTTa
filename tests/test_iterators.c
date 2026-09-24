/* Purpose: verify native producer suspension, failure and deterministic close
 * through both C cursors and engine backtracking.
 * Guarantees: empty, finite, unbounded, interleaved and failing producers close
 * once; late errors never become successful collections
 * [tested: test_native_iterators_close_on_every_exit,
 * test_engine_iterators_keep_arguments_until_close; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 * Owns resources: producer states, cursors, function atoms and runtime.
 */
#include <cmetta.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "allocation_tracker.h"
#define CASE(name) do { fprintf(stderr, "CASE %s\n", (name)); } while (0)

typedef struct sequence {
  uint64_t at, count, fail_at;
  unsigned *closed;
  mt_call *call;
} sequence;

static mt_status next(void *state, mt_atom **answer)
{ sequence *s = state;
  if ( s->call ) assert((uint64_t)mt_int(mt_arg(s->call, 0)) == s->count);
  if ( s->at == s->fail_at ) return mt_error_set(MT_ERROR, "producer broke");
  if ( s->at == s->count ) return MT_DONE;
  *answer = mt_unum(s->at++);
  return *answer ? MT_ROW : MT_NOMEM;
}

static void close_sequence(void *state)
{ sequence *s = state;
  (*s->closed)++;
  mt_free(s);
}

static mt_iterator sequence_open(uint64_t count, uint64_t fail_at,
                                  unsigned *closed, mt_call *call)
{ sequence *s = mt_alloc(sizeof(*s));
  assert(s);
  *s = (sequence){0, count, fail_at, closed, call};
  return (mt_iterator){s, next, close_sequence};
}

static mt_status generator(mt_call *call, void *user)
{ int64_t count = mt_int(mt_arg(call, 0));
  if ( count < 0 ) return mt_fail(call, "negative count");
  return mt_answer_iter(call, sequence_open((uint64_t)count, UINT64_MAX, user, call));
}

static mt_status failing_generator(mt_call *call, void *user)
{ return mt_answer_iter(call, sequence_open(2, 1, user, NULL));
}

static void test_native_iterators_close_on_every_exit(void)
{ CASE("native iterators close on every exit"); unsigned closed = 0;
  mt_answers *a;
  const mt_atom *atom;
  mt_list list;
  a = mt_answers_from(sequence_open(3, UINT64_MAX, &closed, NULL));
  assert(a && mt_answers_status(a) == MT_OK);
  for (int i = 0; i < 3; i++)
    assert(mt_step(a, &atom) == MT_ROW && mt_int(atom) == i);
  assert(mt_step(a, &atom) == MT_DONE && !atom && closed == 1);
  assert(mt_step(a, &atom) == MT_DONE && closed == 1);
  mt_answers_free(a);
  assert(closed == 1);
  a = mt_answers_from(sequence_open(UINT64_MAX, UINT64_MAX, &closed, NULL));
  atom = mt_first(a);
  assert(atom && mt_int(atom) == 0 && closed == 2);
  mt_drop(atom);
  mt_clear();
  assert(!mt_one(mt_answers_from(sequence_open(2, 1, &closed, NULL))));
  assert(mt_error() == MT_ERROR && closed == 3);
  mt_clear();
  list = mt_all(mt_answers_from(sequence_open(3, 2, &closed, NULL)));
  assert(!list.items && !list.len && mt_error() == MT_ERROR && closed == 4);
  mt_clear();
  mt_answers_free(mt_answers_from(sequence_open(0, UINT64_MAX, &closed, NULL)));
  assert(closed == 5);
}

static void test_engine_iterators_keep_arguments_until_close(void)
{ CASE("engine iterators keep arguments until close"); unsigned closed = 0;
  metta *m = mt_open(NULL);
  mt_list list;
  mt_answers *a, *b;
  mt_atom *function;
  const mt_atom *atom;
  assert(m);
  assert(mt_def(m, (mt_op){"c-range", 1, MT_EFFECT_CLASS_NONDETERMINISTIC_READ_ONLY, generator, &closed}));
  assert(mt_def(m, (mt_op){"c-failure", 0, MT_EFFECT_CLASS_NONDETERMINISTIC_READ_ONLY, failing_generator, &closed}));
  list = mt_all(mt_eval(m, mt_expr("c-range", 4)));
  assert(list.len == 4 && closed == 1);
  for (size_t i = 0; i < list.len; i++) assert(mt_int(list.items[i]) == (int64_t)i);
  mt_list_free(list);
  list = mt_all(mt_eval(m, mt_expr("c-range", 0)));
  assert(!list.len && closed == 2);
  mt_list_free(list);
  a = mt_eval(m, mt_expr("c-range", 1000000));
  b = mt_eval(m, mt_expr("c-range", 2));
  assert(mt_int(mt_next(a)) == 0 && mt_int(mt_next(b)) == 0);
  assert(mt_int(mt_next(a)) == 1 && mt_int(mt_next(b)) == 1);
  mt_answers_free(a);
  mt_answers_free(b);
  assert(closed == 4);
  function = mt_function(generator, &closed, NULL);
  list = mt_all(mt_eval(m, mt_expr("c-iter", mt_expr(function, 3))));
  assert(list.len == 3 && closed == 5);
  mt_list_free(list);
  /* Backtracking with a bound result must skip earlier non-matching rows. */
  assert(mt_do(m, "!(assertEqual (collapse (c-range 3)) (0 1 2))"));
  assert(closed == 6);
  mt_clear();
  list = mt_all(mt_eval(m, mt_exprv(1, (mt_atom *[]){mt_sym("c-failure")})));
  assert(!list.len && !list.items && closed == 7 && mt_error() == MT_ERROR);
  assert(strstr(mt_errmsg(), "producer broke"));
  mt_clear();
  a = mt_eval(m, mt_expr("c-range", 2));
  assert(mt_step(a, &atom) == MT_ROW && mt_int(atom) == 0);
  assert(mt_undef(m, "c-range"));
  assert(mt_step(a, &atom) == MT_ROW && mt_int(atom) == 1);
  mt_answers_free(a);
  assert(closed == 8);
  function = mt_function(failing_generator, &closed, NULL);
  list = mt_all(mt_eval(m, mt_expr("c-iter", mt_expr(function))));
  assert(!list.items && !list.len && closed == 9 && mt_error() == MT_ERROR);
  assert(strstr(mt_errmsg(), "producer broke"));
  mt_clear();
  mt_close(m);
}

int main(int argc, char **argv)
{ tracker allocation = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &allocation});
  test_native_iterators_close_on_every_exit();
  mt_clear();
  if ( argc == 1 ) test_engine_iterators_keep_arguments_until_close();
  else assert(argc == 2 && strcmp(argv[1], "--native") == 0);
  assert(!allocation.blocks && !allocation.bytes);
  mt_allocator_set(previous);
  puts("iterators: suspension, interleaving, close, callable values and late refusal passed");
  return 0;
}
