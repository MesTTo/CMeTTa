/* Purpose: prove a native thread can attach to the embedded engine, call a
 *   previously published C operation, keep its diagnostics isolated from a
 *   concurrent caller, and detach without damaging the main engine.
 * Assumes: mt_def runs before pthread_create; cmetta.h documents the operation
 *   table as unguarded after worker evaluation starts.
 * Guarantees: exits 0 only after two attached workers have received their own
 *   error text, exercised mt_of, isolated the mt_show ring, and detached
 *   [tested: tests/test_threads; commit=b339084bb5625996fc88a31608d48ad31c575d1f].
 * Owns resources: two pthreads and their joined lifetimes; one runtime closed
 *   after both workers have detached.
 * Guarded by: C atomics coordinate rendezvous; each worker owns its result.
 */

#define _POSIX_C_SOURCE 200809L
#define MT_SHORTHAND
#include <cmetta.h>

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define ROUNDS 32
#define RENDEZVOUS_SPINS 10000000U

typedef struct thread_test
{ metta *runtime;
  atomic_uint arrivals;
  atomic_uint attached;
  atomic_bool attach_failed;
} thread_test;

typedef struct worker
{ thread_test *test;
  const char  *marker;
  bool         churn_show_ring;
  int          failures;
} worker;

static bool rendezvous(thread_test *test)
{ unsigned arrival = atomic_fetch_add_explicit(&test->arrivals, 1,
                                                memory_order_acq_rel) + 1;
  unsigned pair = (arrival + 1U) & ~1U;
  unsigned spins;

  for (spins = 0; spins < RENDEZVOUS_SPINS; spins++)
  { if ( atomic_load_explicit(&test->arrivals, memory_order_acquire) >= pair )
      return true;
    sched_yield();
  }
  return false;
}

static mt_status thread_failure(mt_call *call, void *user)
{ thread_test *test = user;
  const mt_atom *argument = mt_arg(call, 0);
  const char *marker = mt_name(argument);

  if ( mt_of(call) != test->runtime )
    return mt_fail(call, "mt_of returned another runtime");
  if ( !marker ) return mt_fail(call, "thread-fail wants a marker symbol");
  if ( !rendezvous(test) )
    return mt_fail(call, "the concurrent callback rendezvous was not reached");
  return mt_fail(call, marker);
}

static void fail(worker *w, const char *what)
{ fprintf(stderr, "%s: %s (status %s; %s)\n", w->marker, what,
          mt_status_str(mt_error()), mt_errmsg() ? mt_errmsg() : "no message");
  w->failures++;
}

static void *run_worker(void *opaque)
{ worker *w = opaque;
  unsigned i;

  if ( !mt_thread_attach() )
  { atomic_store_explicit(&w->test->attach_failed, true, memory_order_release);
    atomic_fetch_add_explicit(&w->test->attached, 1, memory_order_acq_rel);
    fail(w, "mt_thread_attach failed");
    return NULL;
  }
  atomic_fetch_add_explicit(&w->test->attached, 1, memory_order_acq_rel);
  while ( atomic_load_explicit(&w->test->attached, memory_order_acquire) < 2 )
    sched_yield();
  if ( atomic_load_explicit(&w->test->attach_failed, memory_order_acquire) )
  { mt_thread_detach();
    return NULL;
  }

  for (i = 0; i < ROUNDS; i++)
  { char source[96];
    mt_answers *answers;

    snprintf(source, sizeof(source), "!(thread-fail %s)", w->marker);
    mt_clear();
    answers = mt_run(w->test->runtime, source);
    if ( answers || mt_error() != MT_ERROR || !mt_errmsg() ||
         strstr(mt_errmsg(), w->marker) == NULL )
      fail(w, "a concurrent operation reported another thread's reason");
    mt_answers_free(answers);
  }

  { mt_atom *marker = S(w->marker);
    const char *held = mt_show(marker);

    if ( !held || !rendezvous(w->test) )
      fail(w, "the show-ring rendezvous failed");
    if ( w->churn_show_ring )
    { unsigned slot;
      for (slot = 0; slot < MT_SHOW_SLOTS * 3; slot++)
      { char name[48];
        mt_atom *atom;
        snprintf(name, sizeof(name), "other-thread-%u", slot);
        atom = S(name);
        (void)mt_show(atom);
        mt_drop(atom);
      }
    }
    if ( !rendezvous(w->test) ) fail(w, "the show-ring release rendezvous failed");
    if ( !w->churn_show_ring && strcmp(held, w->marker) != 0 )
      fail(w, "another thread overwrote this thread's mt_show ring");
    mt_drop(marker);
  }

  mt_thread_detach();
  return NULL;
}

int main(void)
{ thread_test test = {0};
  worker workers[2] = {
    { .test = &test, .marker = "thread-alpha", .churn_show_ring = false },
    { .test = &test, .marker = "thread-beta",  .churn_show_ring = true }
  };
  pthread_t threads[2];
  unsigned created = 0;
  int failed = 0;

  test.runtime = mt_open(NULL);
  if ( !test.runtime )
  { fprintf(stderr, "thread test boot failed: %s\n", mt_errmsg());
    return 1;
  }
  if ( !mt_def(test.runtime,
               (mt_op){ .name = "thread-fail", .arity = 1,
                        .effect = MT_PURE, .fn = thread_failure,
                        .user = &test }) )
  { fprintf(stderr, "thread operation publish failed: %s\n", mt_errmsg());
    mt_close(test.runtime);
    return 1;
  }

  for (created = 0; created < 2; created++)
  { if ( pthread_create(&threads[created], NULL, run_worker,
                        &workers[created]) != 0 )
    { fprintf(stderr, "pthread_create failed for worker %u\n", created);
      break;
    }
  }
  if ( created != 2 )
  { atomic_store_explicit(&test.attach_failed, true, memory_order_release);
    atomic_store_explicit(&test.attached, 2, memory_order_release);
  }
  while ( created > 0 )
  { created--;
    if ( pthread_join(threads[created], NULL) != 0 ) failed++;
  }
  failed += workers[0].failures + workers[1].failures;

  mt_clear();
  if ( mt_one_int(mt_run(test.runtime, "!(+ 20 22)")) != 42 || !mt_ok() )
  { fprintf(stderr, "main engine failed after worker detach: %s\n",
            mt_errmsg() ? mt_errmsg() : "no message");
    failed++;
  }
  if ( !mt_undef(test.runtime, "thread-fail") ) failed++;
  mt_close(test.runtime);

  if ( failed == 0 )
    puts("thread attach, isolated errors, mt_of and detach ok");
  return failed == 0 ? 0 : 1;
}
