/* Purpose: exercise allocator provenance, borrowed atoms and allocation-free
 * teardown independently of the engine.
 * Guarantees: fault injection visits every allocation in construction and a
 * deterministic DAG sweep releases every block [tested: test_dag_sweep;
 * commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 * Owns resources: every atom and buffer is released before its allocator state.
 */
#include <cmetta.h>
#include <assert.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "allocation_tracker.h"
#define CASE(name) do { fprintf(stderr, "CASE %s\n", (name)); } while (0)

static void released(void *owner)
{ (*(size_t *)owner)++;
}

static mt_atom *construction(void)
{ mt_atom *a = mt_text("seed");
  for (unsigned i = 0; a && i < 80; i++)
  { mt_atom *children[] = {a, mt_num(i)};
    a = mt_exprv(2, children);
  }
  return a;
}

static void test_allocation_failures(tracker *t)
{ CASE("allocation failures"); mt_atom *a;
  size_t start = t->calls, allocations;
  a = construction();
  assert(a);
  allocations = t->calls - start;
  mt_drop(a);
  assert(t->blocks == 0);
  for (size_t i = 1; i <= allocations; i++)
  { mt_clear();
    t->fail_at = t->calls + i;
    a = construction();
    assert(!a && mt_error() != MT_OK);
    t->fail_at = 0;
    mt_drop(a);
    assert(t->blocks == 0 && t->bytes == 0);
  }
  mt_clear();
  printf("ownership: %zu allocation failure positions released completely\n", allocations);
}

static void test_provenance(tracker *t)
{ CASE("provenance"); tracker other = {0};
  mt_allocator saved;
  unsigned char *bytes = mt_calloc(32, 1), *grown;
  assert(bytes && (uintptr_t)bytes % _Alignof(max_align_t) == 0);
  for (size_t i = 0; i < 32; i++) assert(bytes[i] == 0);
  bytes[0] = 71;
  t->fail_at = t->calls + 1;
  grown = mt_resize(bytes, 128);
  assert(!grown && mt_error() == MT_NOMEM && bytes[0] == 71);
  t->fail_at = 0;
  saved = mt_allocator_set((mt_allocator){tracked_resize, &other});
  grown = mt_resize(bytes, 64);
  assert(grown && grown[0] == 71 && other.calls == 0);
  mt_free(grown);
  mt_allocator_set(saved);
  assert(!t->blocks && !t->bytes);
  assert(!mt_alloc(SIZE_MAX) && mt_error() == MT_NOMEM);
  assert(!mt_calloc(SIZE_MAX, 2) && mt_error() == MT_NOMEM);
  assert(!mt_textn("", SIZE_MAX) && mt_error() == MT_NOMEM);
  mt_clear();
  assert(!mt_alloc(0) && mt_ok());
}

static void test_borrowing(tracker *t)
{ CASE("borrowing"); const char bytes[] = {'a', '\0', 'b', '\0'};
  size_t text_releases = 0, vector_releases = 0, before;
  mt_atom *text = mt_text_ref(bytes, 3, &text_releases, released);
  const mt_atom *children[] = {text, text};
  mt_atom *expr, *owned;
  assert(text && mt_name(text) == bytes && mt_name_len(text) == 3);
  before = t->calls;
  expr = mt_expr_ref(2, children, &vector_releases, released);
  assert(expr && t->calls == before + 1 && mt_children(expr) == children);
  assert(mt_at(expr, 1) == text);
  owned = mt_atom_of((const mt_atom *)text);
  mt_drop(text);
  mt_drop(expr);
  assert(vector_releases == 1 && !text_releases);
  mt_drop(owned);
  assert(text_releases == 1 && !t->blocks);
  t->fail_at = t->calls + 1;
  text = mt_text_ref(bytes, 3, &text_releases, released);
  assert(!text && text_releases == 1);
  t->fail_at = 0;
  assert(!mt_expr_ref(1, NULL, NULL, NULL));
  assert(!mt_text_ref(NULL, 0, NULL, NULL));
  mt_clear();
}

static void test_teardown(tracker *t)
{ CASE("teardown"); mt_atom *a = mt_text("leaf");
  size_t calls;
  for (size_t i = 0; i < 100000; i++)
  { mt_atom *children[] = {a, mt_keep(a)};
    a = mt_exprv(2, children);
    assert(a);
  }
  calls = t->calls;
  t->fail_at = calls + 1;
  mt_drop(a);
  assert(t->calls == calls && !t->blocks && !t->bytes && mt_ok());
  t->fail_at = 0;
}

static void test_dag_sweep(tracker *t)
{ CASE("dag sweep"); enum { N = 4096 };
  mt_atom *nodes[N];
  uint32_t random = 0x193713;
  for (size_t i = 0; i < N; i++)
  { random = random * 1664525u + 1013904223u;
    if ( !i || (random & 7) == 0 ) nodes[i] = mt_unum(random);
    else
    { mt_atom *children[] = {mt_keep(nodes[random % i]),
                            mt_keep(nodes[(random >> 8) % i])};
      nodes[i] = mt_exprv(2, children);
    }
    assert(nodes[i]);
  }
  for (size_t i = 0; i < N; i++) mt_drop(nodes[i]);
  assert(!t->blocks && !t->bytes);
}

static void test_numeric_conversion(void)
{ CASE("numeric conversion"); mt_atom *a = mt_atom_of(UINT64_MAX);
  assert(a && mt_kind_of(a) == MT_BIGINT);
  assert(strcmp(mt_name(a), "18446744073709551615") == 0);
  mt_drop(a);
  a = mt_unum(INT64_MAX);
  assert(a && mt_int(a) == INT64_MAX);
  mt_drop(a);
#if LDBL_MANT_DIG > DBL_MANT_DIG
  /* Valgrind models x87 arithmetic at 64 bits; measure the value delivered
     by this execution before requiring an extended-precision refusal.
     Native execution still checks the nonrepresentable value.
     [source: Valgrind manual-core.html, Limitations, x86/AMD64 floating point;
     commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c] */
  volatile long double wider = 1.0L + LDBL_EPSILON;
  a = mt_atom_of(wider);
  if ( wider > 1.0L )
  { assert(!a && mt_error() == MT_UNSUPPORTED);
    mt_clear();
  } else
  { assert(a && mt_float(a) == 1.0);
    mt_drop(a);
    puts("ownership: execution rounded long double to double; extended precision is checked natively");
  }
#endif
}

static mt_status unused_function(mt_call *call, void *user)
{ (void)call; (void)user; return MT_FAIL; }

static void test_box_failures(tracker *t)
{ CASE("box failures"); for (unsigned callable = 0; callable < 2; callable++)
  { size_t count = 0, before = t->calls, allocations;
    mt_atom *atom = callable ? mt_function(unused_function, &count, released)
                             : mt_object(&count, "Owned", released);
    assert(atom);
    allocations = t->calls - before;
    mt_drop(atom);
    assert(count == 1 && !t->blocks);
    for (size_t i = 1; i <= allocations; i++)
    { t->fail_at = t->calls + i;
      atom = callable ? mt_function(unused_function, &count, released)
                      : mt_object(&count, "Owned", released);
      assert(!atom && mt_error() == MT_NOMEM && count == i + 1);
      t->fail_at = 0;
      assert(!t->blocks && !t->bytes);
      mt_clear();
    }
  }
  { size_t count = 0;
    assert(!mt_function(NULL, &count, released));
    assert(count == 1 && mt_error() == MT_MISUSE);
    mt_clear();
  }
}

int main(void)
{ tracker t = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &t});
  test_allocation_failures(&t);
  test_provenance(&t);
  test_borrowing(&t);
  test_teardown(&t);
  test_dag_sweep(&t);
  test_numeric_conversion();
  test_box_failures(&t);
  assert(!t.blocks && !t.bytes);
  mt_allocator_set(previous);
  puts("ownership: allocator provenance, borrowed spans, deep teardown, DAG sweep and exact numbers passed");
  return 0;
}
