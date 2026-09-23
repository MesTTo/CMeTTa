/* Purpose: prove a native thread can attach to the embedded engine, call a
 *   previously published C operation, keep its diagnostics isolated from a
 *   concurrent caller, and detach without damaging the main engine.
 * Assumes: mt_def runs before pthread_create; cmetta.h documents the operation
 *   table as unguarded after worker evaluation starts. Links the fault
 *   library, whose mt_test_foreign_handle makes the native handles the drop
 *   test needs, since the public surface cannot make one.
 * Guarantees: exits 0 only after two attached workers have received their own
 *   error text, exercised mt_of, isolated the mt_show ring, and detached
 *   [tested: test_threads.c; commit=b339084bb5625996fc88a31608d48ad31c575d1f],
 *   and after four threads dropped 4,000 handles while the main thread closed
 *   the runtime [tested: test_threads.c, test_drop_handles_while_closing;
 *   commit=e14d01465d3e233d5cb5ccd1fc9c685c20c70000], and after a thread with
 *   no engine dropped twice the atom-GC margin in handles, erasing none of
 *   their records itself, and the main thread's next door erased them all
 *   [tested: test_threads.c, test_handles_dropped_without_an_engine;
 *   commit=WORKTREE].
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
#include <stdlib.h>
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

/* Handles dropped on worker threads while the main thread closes the
   runtime: each drop erases the handle's engine record unless the close has
   begun, and none may erase into the heap PL_cleanup() is freeing. This is a
   safety smoke test and not a discriminating one: the window is narrow, and
   a library without the close handshake passed it 12 runs of 12 too
   [measured 2026-09-24]; test_internal_contracts tests the handshake's rule
   deterministically. */
enum { HANDLES = 4000, DROPPERS = 4 };

/* A handle over a fresh native test blob, from the fault library. */
extern mt_atom *mt_test_foreign_handle(unsigned seed);

typedef struct dropper
{ mt_atom   **handles;
  size_t      first, count;
  atomic_uint *started;
} dropper;

static void *run_dropper(void *opaque)
{ dropper *d = opaque;
  size_t i;
  atomic_fetch_add(d->started, 1);
  for (i = d->first; i < d->first + d->count; i++) mt_drop(d->handles[i]);
  return NULL;
}

/* Handles dropped on a thread with no Prolog engine, twice the atom-GC
   margin of them, so the unregister that crosses the margin happens on that
   thread. Erasing there faulted in SWI's signalGCThread(), which reads the
   thread's engine [measured 2026-09-24: 3 runs of 3 at 30,000 handles]. The
   records wait for a thread with an engine instead: the drops erase none,
   and the main thread's next door erases every one. */
extern unsigned mt_test_record_erases(void);
extern int64_t mt_test_agc_margin(void);

static int test_handles_dropped_without_an_engine(metta *runtime)
{ int64_t margin = mt_test_agc_margin();
  size_t count = margin > 0 ? 2 * (size_t)margin : 1, i;
  mt_atom **handles = malloc(count * sizeof *handles);
  atomic_uint started = 0;
  dropper d = { handles, 0, count, &started };
  pthread_t thread;
  unsigned before, during;
  int failed = 0;

  if ( !handles || margin < 0 )
  { fprintf(stderr, "no room for %zu handles, or no agc_margin flag (%lld)\n",
            count, (long long)margin);
    free(handles);
    return 1;
  }
  for (i = 0; i < count; i++)
    if ( mt_kind_of(handles[i] = mt_test_foreign_handle((unsigned)(HANDLES + i))) != MT_HANDLE )
    { fprintf(stderr, "test blob %zu did not decode as a handle\n", i);
      return 1;
    }
  before = mt_test_record_erases();
  if ( pthread_create(&thread, NULL, run_dropper, &d) != 0 ||
       pthread_join(thread, NULL) != 0 )
    return 1;
  during = mt_test_record_erases() - before;
  if ( during != 0 )
  { fprintf(stderr, "a thread with no engine erased %u records itself\n", during);
    failed++;
  }
  if ( mt_one_int(mt_run(runtime, "!(+ 20 22)")) != 42 )
  { fprintf(stderr, "the engine failed after the drops: %s\n",
            mt_errmsg() ? mt_errmsg() : "no message");
    failed++;
  }
  if ( mt_test_record_erases() - before != count )
  { fprintf(stderr, "the next door erased %u of %zu waiting records\n",
            mt_test_record_erases() - before, count);
    failed++;
  }
  free(handles);
  return failed;
}

static int test_drop_handles_while_closing(metta *runtime)
{ static mt_atom *handles[HANDLES];
  dropper droppers[DROPPERS];
  pthread_t threads[DROPPERS];
  atomic_uint started = 0;
  size_t i;
  int failed = 0;

  for (i = 0; i < HANDLES; i++)
  { handles[i] = mt_test_foreign_handle((unsigned)i);
    if ( mt_kind_of(handles[i]) != MT_HANDLE )
    { fprintf(stderr, "test blob %zu did not decode as a handle: %s\n", i,
              mt_errmsg() ? mt_errmsg() : "no message");
      return 1;
    }
  }
  for (i = 0; i < DROPPERS; i++)
  { droppers[i] = (dropper){ handles, i * (HANDLES / DROPPERS), HANDLES / DROPPERS, &started };
    if ( pthread_create(&threads[i], NULL, run_dropper, &droppers[i]) != 0 ) return 1;
  }
  while ( atomic_load(&started) < DROPPERS ) sched_yield();
  mt_close(runtime);                        /* while the drops are in flight */
  for (i = 0; i < DROPPERS; i++)
    if ( pthread_join(threads[i], NULL) != 0 ) failed++;
  return failed;
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
  failed += test_handles_dropped_without_an_engine(test.runtime);
  failed += test_drop_handles_while_closing(test.runtime);
  mt_close(test.runtime);                   /* already closed: a no-op */

  if ( failed == 0 )
    puts("thread attach, isolated errors, mt_of, detach and handle drops "
         "during close ok");
  return failed == 0 ? 0 : 1;
}
