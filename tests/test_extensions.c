/* Purpose: exercise extension initialization as one registration transaction.
 * Guarantees: a refused shared library leaves no registrations or equations;
 * a successful library remains callable [tested: test_extensions; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 * Owns resources: runtime shutdown releases every counted C allocation;
 * shared libraries remain mapped until process exit for escaped callbacks.
 */
#include <cmetta.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "allocation_tracker.h"
#define CASE(name) do { fprintf(stderr, "CASE %s\n", (name)); } while (0)

static int test_extensions(int argc, char **argv)
{ CASE("extension initialization publishes atomically or rolls back"); tracker allocation = {0};
  mt_allocator previous = mt_allocator_set((mt_allocator){tracked_resize, &allocation});
  metta *m = mt_open(NULL);
  size_t ops, libraries, atoms;
  mt_atom *result;
  const char *failed = argc == 3 ? argv[1] : "./tests/extension_refuse.so";
  const char *accepted = argc == 3 ? argv[2] : "./tests/extension_accept.so";
  assert(m && (argc == 1 || argc == 3));
  ops = mt_seam_count(m, "op"); libraries = mt_seam_count(m, "library"); atoms = mt_count(m);
  assert(!mt_extension(m, failed));
  assert(strstr(mt_errmsg(), "mt_extension_init answered false")); mt_clear();
  assert(mt_seam_count(m, "op") == ops && mt_seam_count(m, "library") == libraries);
  assert(mt_count(m) == atoms);
  result = mt_one(mt_eval(m, mt_expr("extension-fixture")));
  assert(result && mt_kind_of(result) == MT_EXPR); mt_drop(result);
  assert(mt_extension(m, accepted));
  assert(mt_one_int(mt_eval(m, mt_expr("extension-fixture"))) == 73);
  assert(mt_one_int(mt_eval(m, mt_expr("extension-fixture-equation"))) == 73);
  assert(mt_seam_count(m, "op") == ops + 1 && mt_seam_count(m, "library") == libraries + 1);
  mt_close(m);
  assert(!allocation.blocks && !allocation.bytes);
  mt_allocator_set(previous);
  puts("extensions: separate shared objects, initialization rollback and live calls passed");
  return 0;
}

int main(int argc, char **argv)
{ return test_extensions(argc, argv);
}
