/* Purpose: be the program `make install-check` compiles against an INSTALLED
 *   libcmetta, knowing nothing about this checkout but what pkg-config says.
 *
 * Assumes: cmetta.h is on the include path and libcmetta on the link path,
 *   both supplied by `pkg-config --cflags --libs cmetta`. It does NOT assume
 *   $METTA_PATH: the whole claim being checked is that an installed library
 *   finds the installed engine on its own, because `make install` bakes the
 *   installed engine's directory into it.
 * Guarantees: checks version 1's borrowed atoms, exact unsigned values, native
 *   cursors and forms through the installed header and shared object, prints
 *   5, and on Linux confirms CPython is absent from the process
 *   [tested: make install-check; commit=WORKTREE].
 * Owns resources: releases atoms, collections, cursors and runtime explicitly.
 * Fails when: the engine tree was not installed beside the library, which is
 *   the failure this exists to catch and the reason it prints mt_errmsg().
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#include <cmetta.h>

#include <stdio.h>
#include <assert.h>
#include <string.h>

_Static_assert(__STDC_VERSION__ >= 201112L,
               "cmetta's pkg-config metadata must select C11 or newer");

typedef struct sequence { unsigned next; bool closed; } sequence;
static mt_status next(void *user, mt_atom **out)
{ sequence *s = user;
  if ( s->next == 2 ) return MT_DONE;
  *out = mt_num(s->next++);
  return *out ? MT_ROW : MT_NOMEM;
}
static void close_sequence(void *user) { ((sequence *)user)->closed = true; }

int main(void)
{ metta *m = mt_open(NULL);
  int64_t answer;
  mt_atom *atom;
  mt_list list;
  sequence sequence = {0};
  const char text[] = "borrowed";

  if ( !m )
  { printf("boot failed: %s\n", mt_errmsg() ? mt_errmsg() : "(no message)");
    return 1;
  }
  assert(strcmp(mt_version(), MT_VERSION) == 0 && strcmp(MT_VERSION, "1.0.0") == 0);
  atom = mt_atom_of(UINT64_MAX);
  assert(atom && strcmp(mt_name(atom), "18446744073709551615") == 0); mt_drop(atom);
  atom = mt_text_ref(text, strlen(text), NULL, NULL);
  assert(atom && mt_name(atom) == text); mt_drop(atom);
  list = mt_all(mt_answers_from((mt_iterator){&sequence, next, close_sequence}));
  assert(list.len == 2 && sequence.closed); mt_list_free(list);
  list = mt_forms("(f) (g)"); assert(list.len == 2); mt_list_free(list);
#ifdef __linux__
  { FILE *maps = fopen("/proc/self/maps", "r");
    char line[4096];
    assert(maps);
    while ( fgets(line, sizeof(line), maps) ) assert(!strstr(line, "libpython"));
    assert(!ferror(maps) && fclose(maps) == 0);
  }
#endif
  answer = mt_one_int(mt_eval(m, mt_expr("+", 2, 3)));
  if ( !mt_ok() )
  { printf("evaluation failed: %s\n", mt_errmsg() ? mt_errmsg() : "(no message)");
    mt_close(m);
    return 1;
  }
  printf("%lld\n", (long long)answer);
  mt_close(m);
  return 0;
}
