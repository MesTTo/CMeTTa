/* Purpose: prove that lazy cursor identifiers never recycle and closed
 *   cursor ownership is retired immediately, including exceptional closes.
 * Assumes: this binary links tests/libcmetta_fault.so, whose test-only getter
 *   exposes the numeric identifier without exposing the SWI engine handle.
 * Guarantees: exits nonzero if an emptied cursor table reuses an identifier
 *   or 1,200 concurrent opens cost materially more engine inferences than
 *   opening and closing the same 1,200 cursors one at a time; repeated closes
 *   retain neither dynamic rows, recorded owners nor registered atoms.
 *   [tested: sh extensions/cmetta/test.sh; commit=WORKTREE]
 * Owns resources: closes every cursor and the runtime before exit.
 */

#define MT_SHORTHAND
#include <cmetta.h>
#include <SWI-Prolog.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int64_t mt_test_cursor_id(const mt_answers *answers);

enum { CURSOR_COUNT = 1200 };

static int failures;

static void expect(int condition, const char *claim)
{ if ( condition ) return;
  failures++;
  fprintf(stderr, "cursor regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
}

static mt_answers *open_source(metta *runtime)
{ return mt_eval(runtime, E("cmetta-cursor-source"));
}

static int query(const char *source)
{ fid_t frame = PL_open_foreign_frame();
  term_t goal = PL_new_term_ref();
  int ok = goal && PL_chars_to_term(source, goal) && PL_call(goal, NULL);
  if ( !ok && PL_exception(0) )
  { PL_write_term(Suser_error, PL_exception(0), 1200, PL_WRT_QUOTED);
    PL_clear_exception();
  }
  PL_discard_foreign_frame(frame);
  return ok;
}

static int census(int64_t *bytes, int64_t *records, int64_t *atoms)
{ fid_t frame = PL_open_foreign_frame();
  term_t av = PL_new_term_refs(3);
  predicate_t pred = PL_predicate("cursor_census", 3, "cmetta_cursor_tests");
  int ok = av && PL_call_predicate(NULL, PL_Q_CATCH_EXCEPTION, pred, av) &&
           PL_get_int64(av, bytes) && PL_get_int64(av + 1, records) &&
           PL_get_int64(av + 2, atoms);
  PL_discard_foreign_frame(frame);
  return ok;
}

static void test_closed_cursor_ownership_is_retired(metta *runtime)
{ const size_t sizes[] = {2000, 10000, 20000};
  int64_t before_bytes, before_records, before_atoms, bytes, records, atoms;
  size_t cell, i;

  expect(query("set_prolog_flag(gc_thread,false),set_prolog_flag(gc,false)"),
         "retention must not rely on automatic clause collection");
  mt_answers_free(open_source(runtime));
  expect(census(&before_bytes, &before_records, &before_atoms),
         "the initial ownership census must succeed");
  for (cell = 0; cell < sizeof(sizes) / sizeof(sizes[0]); cell++)
  { for (i = 0; i < sizes[cell]; i++)
    { mt_answers *cursor = open_source(runtime);
      expect(cursor != NULL, "the retention fixture must open each cursor");
      mt_answers_free(cursor);
    }
    if ( !census(&bytes, &records, &atoms) )
    { expect(0, "the final ownership census must succeed");
      break;
    }
    printf("closed cursors %zu: registry bytes %lld, records %lld, atoms %lld "
           "(initial %lld/%lld/%lld)\n", sizes[cell], (long long)bytes,
           (long long)records, (long long)atoms, (long long)before_bytes,
           (long long)before_records, (long long)before_atoms);
    expect(bytes <= before_bytes,
           "closed cursors must not leave dynamic clause tombstones");
    expect(records == before_records,
           "closed cursors must not retain recorded owners");
    expect(atoms <= before_atoms,
           "closed cursors must not retain registered reference or key atoms");
  }
  expect(query("set_prolog_flag(gc,true),set_prolog_flag(gc_thread,true)"),
         "the retention fixture must restore collection");
}

static void test_free_after_close_and_after_close_error(metta *runtime)
{ mt_answers *cursor = open_source(runtime);
  fid_t frame;
  term_t av;
  predicate_t arm, finish;

  expect(cursor != NULL, "the repeated-close fixture must open");
  expect(query("cmetta_cursor_tests:close_live_cursor_twice"),
         "closing the same recorded owner twice must succeed");
  expect(mt_next(cursor) == NULL && mt_error() == MT_ERROR && mt_errmsg() &&
         strstr(mt_errmsg(), "cmetta_cursor"),
         "a closed engine must retain the named cursor refusal");
  mt_clear();
  mt_answers_free(cursor);
  expect(mt_ok(), "freeing an already closed owner must be quiet");

  cursor = open_source(runtime);
  expect(cursor != NULL, "the exceptional-close fixture must open");
  frame = PL_open_foreign_frame();
  av = PL_new_term_refs(2);
  arm = PL_predicate("arm_erasure_fault", 2, "cmetta_cursor_tests");
  finish = PL_predicate("finish_erasure_fault", 2, "cmetta_cursor_tests");
  if ( av && PL_call_predicate(NULL, PL_Q_CATCH_EXCEPTION, arm, av) )
  { mt_clear();
    mt_answers_free(cursor);
    expect(mt_error() == MT_ERROR && mt_errmsg() &&
           strstr(mt_errmsg(), "cursor_close_probe"),
           "an erase listener's exception must remain visible to C");
    expect(PL_call_predicate(NULL, PL_Q_CATCH_EXCEPTION, finish, av),
           "an exceptional close must erase its owner and destroy its engine");
  } else
  { expect(0, "the exceptional-close listener must be installed");
    mt_answers_free(cursor);
  }
  PL_discard_foreign_frame(frame);
  mt_clear();
}

static void test_cursor_ids_are_monotone_and_constant_cost(metta *runtime)
{ mt_answers **held = calloc(CURSOR_COUNT, sizeof(*held));
  mt_answers *old_runtime_cursor, *old_runtime_row_cursor;
  mt_answers *new_runtime_cursor;
  mt_answers *cursor;
  mt_stats before, after;
  uint64_t concurrent, sequential;
  int64_t first_id, second_id, old_id, new_id;
  size_t i;

  expect(held != NULL, "the test must allocate its cursor array");
  if ( !held ) return;

  cursor = open_source(runtime);
  expect(cursor != NULL, "the first cursor must open");
  first_id = mt_test_cursor_id(cursor);
  mt_answers_free(cursor);

  cursor = open_source(runtime);
  expect(cursor != NULL, "the cursor after an empty table must open");
  second_id = mt_test_cursor_id(cursor);
  expect(first_id > 0 && second_id > first_id,
         "an emptied table must not recycle a stale cursor identifier");
  mt_answers_free(cursor);

  before = mt_stats_now(runtime);
  for (i = 0; i < CURSOR_COUNT; i++)
  { held[i] = open_source(runtime);
    expect(held[i] != NULL, "every concurrent cursor must open");
    if ( !held[i] ) break;
  }
  after = mt_stats_now(runtime);
  concurrent = mt_stats_since(before, after).inferences;
  for (i = 0; i < CURSOR_COUNT; i++) mt_answers_free(held[i]);

  before = mt_stats_now(runtime);
  for (i = 0; i < CURSOR_COUNT; i++)
  { cursor = open_source(runtime);
    expect(cursor != NULL, "every sequential cursor must open");
    mt_answers_free(cursor);
  }
  after = mt_stats_now(runtime);
  sequential = mt_stats_since(before, after).inferences;

  /* Both arms perform the same opens. Holding cursors changes only the table
     population, so a constant-time identifier costs at most a small fixed
     amount per row above the sequential arm. The former max/2 scan exceeds
     this allowance by more than 700,000 inferences. */
  expect(concurrent <= sequential + UINT64_C(4) * CURSOR_COUNT,
         "concurrent opens must not rescan the open-cursor table");

  printf("cursor ids %lld then %lld; %llu concurrent vs %llu sequential "
         "open inferences\n",
         (long long)first_id, (long long)second_id,
         (unsigned long long)concurrent,
         (unsigned long long)sequential);
  free(held);

  old_runtime_cursor = open_source(runtime);
  expect(old_runtime_cursor != NULL,
         "a cursor retained across cleanup must open");
  old_runtime_row_cursor = open_source(runtime);
  expect(old_runtime_row_cursor != NULL,
         "a row cursor retained across cleanup must open");
  old_id = mt_test_cursor_id(old_runtime_cursor);
  mt_close(runtime);

  runtime = mt_open(NULL);
  expect(runtime != NULL, "the runtime must restart for the stale-handle case");
  if ( !runtime )
  { mt_answers_free(old_runtime_cursor);
    mt_answers_free(old_runtime_row_cursor);
    return;
  }
  expect(mt_do(runtime,
               "(= (cmetta-cursor-source) (superpose (1 2 3)))"),
         "the cursor source must be restored after restart");
  new_runtime_cursor = open_source(runtime);
  expect(new_runtime_cursor != NULL,
         "the restarted runtime's cursor must open");
  new_id = mt_test_cursor_id(new_runtime_cursor);

  mt_answers_free(old_runtime_cursor);
  mt_clear();
  expect(mt_row_next(old_runtime_row_cursor) == NULL,
         "a stale row cursor must refuse instead of reaching the new engine");
  expect(mt_error() == MT_MISUSE && mt_errmsg() &&
         strstr(mt_errmsg(), "mt_row_next") != NULL,
         "the stale row refusal must name the door the caller used");
  mt_answers_free(old_runtime_row_cursor);
  mt_clear();
  expect(mt_next(new_runtime_cursor) != NULL,
         "freeing a stale cursor must not close a restarted runtime's cursor");
  expect(mt_ok(), "the restarted cursor must remain error-free");
  mt_answers_free(new_runtime_cursor);
  printf("cursor restart ids %lld then %lld remain generation-safe\n",
         (long long)old_id, (long long)new_id);
}

int main(int argc, char **argv)
{ metta *runtime = mt_open(NULL);
  fid_t frame;
  term_t file;
  int loaded;
  if ( !runtime )
  { fprintf(stderr, "cursor regression could not boot: %s\n",
            mt_errmsg() ? mt_errmsg() : "(none)");
    return 1;
  }

  expect(mt_do(runtime,
               "(= (cmetta-cursor-source) (superpose (1 2 3)))"),
         "the cursor source must be defined");
  frame = PL_open_foreign_frame();
  file = PL_new_term_ref();
  loaded = file && PL_put_atom_chars(file,
             MT_ENGINE_PATH "/extensions/cmetta/tests/cursor_lifecycle.pl") &&
           PL_call_predicate(NULL, PL_Q_CATCH_EXCEPTION,
                             PL_predicate("consult", 1, "user"), file);
  PL_discard_foreign_frame(frame);
  expect(loaded, "the embedded cursor lifecycle fixture must load");
  if ( loaded )
  { test_closed_cursor_ownership_is_retired(runtime);
    if ( argc == 2 && strcmp(argv[1], "--retention") == 0 )
    { mt_close(runtime);
      return failures ? 1 : 0;
    }
    expect(query("run_tests(cmetta_cursor_lifecycle)"),
           "single-winner and exceptional close regressions must pass");
    test_free_after_close_and_after_close_error(runtime);
  }
  test_cursor_ids_are_monotone_and_constant_cost(runtime);
  mt_close(runtime);
  return failures ? 1 : 0;
}
