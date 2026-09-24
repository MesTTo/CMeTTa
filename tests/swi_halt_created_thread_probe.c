/* Purpose: reproduce, without cmetta or PeTTa, SWI-Prolog's halt passing over
 *   a thread that has been created but has not started running. thread_create
 *   marks the thread PL_THREAD_CREATED before pthread_create, and the thread
 *   marks itself PL_THREAD_RUNNING only once initialise_thread has returned.
 *   exitPrologThreads() joins the threads that have finished and signals the
 *   running ones, but takes no action on a CREATED one: it is neither
 *   signalled nor awaited. That thread then starts its goal while PL_cleanup()
 *   frees the module tables its callProlog() is looking the goal up in
 *   [source: swipl-devel src/pl-thread.c, thread_create's
 *   `info->status = PL_THREAD_CREATED`, start_thread's
 *   `info->status = PL_THREAD_RUNNING`, and exitPrologThreads()'s
 *   `default: break;`; tag V10.1.14].
 * Assumes: the SWI headers and library the C seat builds against.
 * Guarantees: exits 0 on a host whose halt waits for a thread still being
 *   created, and dies of SIGSEGV or an assertion in cleanup on one that does
 *   not, often enough that make runtime-halt-created-thread's twenty runs do
 *   not all pass on it [measured 2026-09-24: SWI-Prolog 10.1.14 at
 *   swipl-patched.2, 8 threads, 16 runs of 20 died in PL_cleanup; the core
 *   shows the main thread in unallocModule and a new thread in start_thread,
 *   callProlog, stripModule, lookupModule].
 *   A MeTTa program reaches it through lib_thread whenever a timer or a
 *   spawn is created shortly before the runtime closes.
 * Owns resources: the threads it creates are detached and end with the
 *   process's cleanup.
 */
#include <SWI-Prolog.h>
/* The checks below are assert()s, so they stay live whatever NDEBUG a build sets. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Few threads, so the last ones are still being created when cleanup starts. */
enum { THREADS = 8 };

int main(void)
{ char *options[] = {"swipl", "-q", "--nosignals", "--no-packs", NULL};
  char text[128];
  fid_t frame;
  term_t goal;

  assert(PL_initialise(4, options));
  frame = PL_open_foreign_frame();
  goal = PL_new_term_ref();
  snprintf(text, sizeof text,
           "forall(between(1, %d, _), thread_create(true, _, [detached(true)]))",
           THREADS);
  assert(PL_chars_to_term(text, goal) && PL_call(goal, NULL));
  PL_discard_foreign_frame(frame);
  assert(PL_cleanup(0) == PL_CLEANUP_SUCCESS);
  printf("halted past %d threads just created\n", THREADS);
  return 0;
}
