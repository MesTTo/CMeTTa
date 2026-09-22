/* Purpose: isolate SWI allocation failures without loading cmetta or PeTTa.
 * Assumes: the same SWI headers and library used by the C seat.
 * Guarantees: baseline, int64 and Unicode cases differ only in their named
 *   foreign-interface calls [tested: make runtime-memory; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c].
 * Owns resources: discards each foreign frame and requires successful cleanup.
 */
#include <SWI-Prolog.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(int argc, char **argv)
{ char *options[] = {"swipl", "-q", "--nosignals", "--no-packs", NULL};
  assert(argc == 2 && PL_initialise(4, options));
  if (strcmp(argv[1], "baseline") != 0)
  { for (int i = 0; i < 3; i++)
    { fid_t frame = PL_open_foreign_frame();
      term_t args = PL_new_term_refs(2);
      if (strcmp(argv[1], "int64") == 0)
      { assert(PL_put_int64(args, INT64_MAX));
        assert(PL_put_int64(args + 1, INT64_MIN));
      } else
      { assert(strcmp(argv[1], "unicode") == 0);
        assert(PL_put_chars(args, PL_ATOM | REP_UTF8, (size_t)-1, "日本語"));
        assert(PL_call_predicate(NULL, PL_Q_NODEBUG | PL_Q_CATCH_EXCEPTION,
                                 PL_predicate("term_to_atom", 2, NULL), args));
      }
      PL_discard_foreign_frame(frame);
    }
  }
  assert(PL_cleanup(0) == PL_CLEANUP_SUCCESS);
  return 0;
}
