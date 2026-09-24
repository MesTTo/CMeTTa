/* Purpose: reproduce, without cmetta or PeTTa, SWI-Prolog erasing records on
 *   a thread that has no Prolog engine. Erasing unregisters the atoms a
 *   record holds, and the unregister that crosses the atom-GC margin signals
 *   the collector through the calling thread's engine, which is NULL there,
 *   though the manual lets such a thread call anything that takes no term_t.
 * Assumes: the SWI headers and library the C seat builds against.
 * Guarantees: exits 0 on a host whose signalGCThread() tolerates a thread
 *   with no engine, and dies of SIGSEGV on one that does not [measured
 *   2026-09-24, make runtime-engineless-erase: SWI-Prolog 10.1.14 without
 *   the swi-gc-signal-engineless-thread patch died 3 runs of 3, and
 *   swipl-patched.2, which carries it, erased 20,000 records and exited 0 3
 *   runs of 3]. cmetta's handle_release erases on such a thread, so it
 *   needs a host this exits 0 on, as the engine requires since superproject
 *   79a48d315.
 * Owns resources: every record it makes is erased; cleanup must succeed.
 */
#include <SWI-Prolog.h>
/* The checks below are assert()s, so they stay live whatever NDEBUG a build sets. */
#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static record_t *records;
static size_t count;

static void *erase_all(void *unused)
{ (void)unused;
  assert(PL_thread_self() < 0);           /* the point: no engine here */
  for (size_t i = 0; i < count; i++) PL_erase(records[i]);
  return NULL;
}

int main(void)
{ char *options[] = {"swipl", "-q", "--nosignals", "--no-packs", NULL};
  atom_t flag;
  int64_t margin = 0;
  pthread_t thread;

  assert(PL_initialise(4, options));
  flag = PL_new_atom("agc_margin");
  assert(PL_current_prolog_flag(flag, PL_INTEGER, &margin) && margin > 0);
  PL_unregister_atom(flag);
  /* Twice the margin, so the crossing happens on the erasing thread. */
  count = 2 * (size_t)margin;
  assert((records = malloc(count * sizeof *records)));
  { fid_t frame = PL_open_foreign_frame();
    term_t t = PL_new_term_ref();
    char name[48];
    for (size_t i = 0; i < count; i++)
    { snprintf(name, sizeof name, "engineless-erase-%zu", i);
      assert(PL_put_atom_chars(t, name) && (records[i] = PL_record(t)));
    }
    PL_discard_foreign_frame(frame);
  }
  assert(pthread_create(&thread, NULL, erase_all, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  free(records);
  assert(PL_cleanup(0) == PL_CLEANUP_SUCCESS);
  printf("erased %zu records on a thread with no engine\n", count);
  return 0;
}
