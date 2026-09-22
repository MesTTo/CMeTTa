/* Purpose: account for every C allocation and inject one allocation refusal.
 * Owns resources: libc blocks released through the same allocator callback.
 * Guarded by: atomic counters permit releases on the engine's collector thread;
 * fail_at is configured only while the test owns allocation activity.
 * Guarantees: zero blocks and bytes after teardown means all allocations made
 * through this allocator were released [tested: tests/test_ownership.c,
 * tests/test_transactions.c; commit=1a60e2a3cce69d5d6bda67100186939d707f4397].
 */
#ifndef CMETTA_ALLOCATION_TRACKER_H
#define CMETTA_ALLOCATION_TRACKER_H
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct tracker {
  _Atomic size_t blocks, bytes, calls, fail_at;
} tracker;

static void *tracked_resize(void *user, void *pointer, size_t old, size_t size)
{ tracker *t = user;
  void *result;
  if ( !size )
  { assert(pointer);
    assert(atomic_fetch_sub(&t->blocks, 1) > 0);
    assert(atomic_fetch_sub(&t->bytes, old) >= old);
    free(pointer);
    return NULL;
  }
  if ( atomic_fetch_add(&t->calls, 1) + 1 == t->fail_at ) return NULL;
  result = realloc(pointer, size);
  if ( result )
  { if ( size >= old ) atomic_fetch_add(&t->bytes, size - old);
    else assert(atomic_fetch_sub(&t->bytes, old - size) >= old - size);
    if ( !old ) atomic_fetch_add(&t->blocks, 1);
  }
  return result;
}
#endif
