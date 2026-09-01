/* Purpose: prove that lazy cursor identifiers never recycle and opening one
 *   does not scan the table of cursors already open.
 * Assumes: this binary links tests/libcmetta_fault.so, whose test-only getter
 *   exposes the numeric identifier without exposing the SWI engine handle.
 * Guarantees: exits nonzero if an emptied cursor table reuses an identifier
 *   or 1,200 concurrent opens cost materially more engine inferences than
 *   opening and closing the same 1,200 cursors one at a time.
 * Owns resources: closes every cursor and the runtime before exit.
 */

#define MT_SHORTHAND
#include <cmetta.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

static void test_cursor_ids_are_monotone_and_constant_cost(metta *runtime)
{ mt_answers **held = calloc(CURSOR_COUNT, sizeof(*held));
  mt_answers *old_runtime_cursor;
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
  old_id = mt_test_cursor_id(old_runtime_cursor);
  mt_close(runtime);

  runtime = mt_open(NULL);
  expect(runtime != NULL, "the runtime must restart for the stale-handle case");
  if ( !runtime )
  { mt_answers_free(old_runtime_cursor);
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
  expect(mt_next(new_runtime_cursor) != NULL,
         "freeing a stale cursor must not close a restarted runtime's cursor");
  expect(mt_ok(), "the restarted cursor must remain error-free");
  mt_answers_free(new_runtime_cursor);
  printf("cursor restart ids %lld then %lld remain generation-safe\n",
         (long long)old_id, (long long)new_id);
}

int main(void)
{ metta *runtime = mt_open(NULL);
  if ( !runtime )
  { fprintf(stderr, "cursor regression could not boot: %s\n",
            mt_errmsg() ? mt_errmsg() : "(none)");
    return 1;
  }

  expect(mt_do(runtime,
               "(= (cmetta-cursor-source) (superpose (1 2 3)))"),
         "the cursor source must be defined");
  test_cursor_ids_are_monotone_and_constant_cost(runtime);
  mt_close(runtime);
  return failures ? 1 : 0;
}
