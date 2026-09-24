/* Purpose: a separately compiled consumer of the public extension ABI.
 * Guarantees: initialization publishes a callable, an equation and a library
 * together, or refuses after publication to exercise rollback
 * [tested: tests/test_extensions.c; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 */
#include <cmetta.h>

static mt_status answer(mt_call *call, void *user)
{ (void)user; return mt_answer(call, mt_num(73)); }

bool mt_extension_init(metta *runtime)
{ if ( !mt_def(runtime, (mt_op){"extension-fixture", 0, MT_EFFECT_CLASS_PURE_STRUCTURAL, answer, NULL}) ||
       !mt_do(runtime, "(= (extension-fixture-equation) 73)") ||
       !mt_library(runtime, "c_fixture", "./tests/fixtures") ) return false;
#ifdef CMETTA_FIXTURE_REFUSE
  return false;
#else
  return true;
#endif
}
