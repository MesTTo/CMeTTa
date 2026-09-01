/* Purpose: prove that mt_add_all transfers one owned list through one engine
 *   batch while preserving order, multiplicity and refusal atomicity.
 * Assumes: inference counters are deterministic for the same engine calls.
 * Guarantees: exits nonzero unless empty, space and runtime batches work; a
 *   NULL member is refused before any write and releases every owned atom;
 *   and 2,000 writes use one bridge call with fewer than half the sequential
 *   engine inferences.
 * Owns resources: closes its spaces and runtime and transfers every list to
 *   mt_add_all, including the refused list.
 */

#define MT_SHORTHAND
#include <cmetta.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MEASURED_ATOMS = 2000 };

static int failures;

static void expect(bool condition, const char *claim)
{ if ( condition ) return;
  failures++;
  fprintf(stderr, "batch-add regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
}

static mt_list allocated_list(size_t len)
{ mt_list list = {0};
  if ( len ) list.items = calloc(len, sizeof(*list.items));
  if ( list.items ) list.len = len;
  else if ( len ) failures++;
  return list;
}

static mt_list numbered_list(size_t len)
{ mt_list list = allocated_list(len);
  size_t i;
  for (i = 0; i < list.len; i++)
  { list.items[i] = N((int64_t)i);
    if ( !list.items[i] )
    { failures++;
      mt_list_free(list);
      return (mt_list){0};
    }
  }
  return list;
}

static void count_release(void *value)
{ (*(int *)value)++;
}

static void test_batch_semantics(metta *runtime)
{ mt_space *space = mt_space_open(runtime, "&cmetta-batch-contract");
  mt_list list;
  mt_answers *answers;
  const mt_atom *atom;
  mt_atom *found;
  int releases = 0;
  size_t before;

  expect(space != NULL, "the batch test space must open");
  if ( !space ) return;
  expect(mt_wipe(space), "the batch test space must start empty");

  expect(mt_add_all(space, (mt_list){0}), "an empty batch must be valid");
  expect(mt_count(space) == 0, "an empty batch must write nothing");

  list = allocated_list(3);
  if ( list.items )
  { list.items[0] = S("batch-a");
    list.items[1] = S("batch-b");
    list.items[2] = S("batch-b");
    expect(mt_add_all(space, list), "one batch must be accepted");
  }
  expect(mt_count(space) == 3, "a batch must preserve multiplicity");
  answers = mt_atoms(space);
  atom = mt_next(answers);
  expect(atom && strcmp(mt_name(atom), "batch-a") == 0,
         "the first batched atom must retain its order");
  atom = mt_next(answers);
  expect(atom && strcmp(mt_name(atom), "batch-b") == 0,
         "the second batched atom must retain its order");
  atom = mt_next(answers);
  expect(atom && strcmp(mt_name(atom), "batch-b") == 0,
         "the duplicate batched atom must remain present");
  expect(mt_next(answers) == NULL && mt_ok(),
         "the batch atom walk must end without an error");
  mt_answers_free(answers);

  before = mt_count(space);
  list = allocated_list(3);
  if ( list.items )
  { list.items[0] = mt_object(&releases, "batch-release", count_release);
    list.items[1] = NULL;
    list.items[2] = S("must-not-be-written");
    mt_clear();
    expect(!mt_add_all(space, list), "a NULL batch member must be refused");
    expect(mt_error() == MT_MISUSE,
           "a NULL batch member must be reported as misuse");
  }
  expect(releases == 1, "a refused batch must release its owned object");
  expect(mt_count(space) == before,
         "a refused batch must make no partial space write");

  list = allocated_list(1);
  if ( list.items )
  { list.items[0] = S("batch-self-receiver");
    expect(mt_add_all(runtime, list),
           "the generic batch door must accept a runtime receiver");
  }
  found = mt_first(mt_match(runtime, S("batch-self-receiver")));
  expect(found != NULL, "the runtime receiver must write to &self");
  mt_drop(found);
  expect(mt_del(runtime, S("batch-self-receiver")),
         "the runtime receiver fixture must be removable");

  expect(mt_wipe(space), "the batch test space must clean up");
  mt_space_close(space);
}

static void test_batch_inference_cost(metta *runtime)
{ mt_space *sequential_space = mt_space_open(runtime, "&cmetta-batch-sequential");
  mt_space *batch_space = mt_space_open(runtime, "&cmetta-batch-single-call");
  mt_stats before, after;
  uint64_t sequential = 0, batch = 0;
  mt_list list;
  size_t i;

  expect(sequential_space && batch_space, "both measurement spaces must open");
  if ( !sequential_space || !batch_space ) goto done;
  expect(mt_wipe(sequential_space) && mt_wipe(batch_space),
         "the measurement spaces must start empty");

  before = mt_stats_now(runtime);
  for (i = 0; i < MEASURED_ATOMS; i++)
    expect(mt_add(sequential_space, N((int64_t)i)),
           "every sequential write must succeed");
  after = mt_stats_now(runtime);
  sequential = mt_stats_since(before, after).inferences;

  list = numbered_list(MEASURED_ATOMS);
  before = mt_stats_now(runtime);
  expect(list.len == MEASURED_ATOMS && mt_add_all(batch_space, list),
         "the measured batch write must succeed");
  after = mt_stats_now(runtime);
  batch = mt_stats_since(before, after).inferences;

  expect(mt_count(sequential_space) == MEASURED_ATOMS &&
         mt_count(batch_space) == MEASURED_ATOMS,
         "both measured arms must write the same number of atoms");
  expect(batch * 2 < sequential,
         "one batch call must use less than half the sequential inferences");

  printf("batch add %d atoms: %llu sequential vs %llu batch inferences\n",
         MEASURED_ATOMS, (unsigned long long)sequential,
         (unsigned long long)batch);
done:
  if ( sequential_space )
  { mt_wipe(sequential_space);
    mt_space_close(sequential_space);
  }
  if ( batch_space )
  { mt_wipe(batch_space);
    mt_space_close(batch_space);
  }
}

int main(void)
{ metta *runtime = mt_open(NULL);
  expect(runtime != NULL, "the runtime must boot");
  if ( !runtime ) return 1;
  test_batch_semantics(runtime);
  test_batch_inference_cost(runtime);
  mt_close(runtime);
  return failures ? 1 : 0;
}
