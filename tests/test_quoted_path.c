/* Purpose: prove that the engine directory is passed to consult/1 as data.
 * Assumes: CMETTA_TEST_ENGINE_PATH names a symlink to the source tree and its
 *   path contains an apostrophe and a non-ASCII character.
 * Guarantees: exits nonzero if that ordinary directory name cannot boot and
 *   evaluate through mt_open().
 * Owns resources: closes the successfully opened runtime before exit; the
 *   Makefile owns and removes the symlink fixture.
 */

#define MT_SHORTHAND
#include <cmetta.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *claim)
{ fprintf(stderr, "quoted path regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
  return 1;
}

static int test_engine_path_is_passed_as_data(void)
{ const char *path = getenv("CMETTA_TEST_ENGINE_PATH");
  mt_config config = {0};
  metta *runtime;
  int failed = 0;

  if ( !path || !*path )
    return fail("CMETTA_TEST_ENGINE_PATH must name the fixture");
  if ( !strchr(path, '\'' ) )
    return fail("the fixture path must contain an apostrophe");
  if ( !strstr(path, "unicod\xc3\xa9") )
    return fail("the fixture path must contain its UTF-8 test character");

  config.path = path;
  runtime = mt_open(&config);
  if ( !runtime ) return fail("mt_open must accept the fixture path");
  if ( mt_one_int(mt_run(runtime, "!(+ 20 22)")) != 42 )
    failed |= fail("the runtime opened through the fixture must evaluate");
  if ( !mt_ok() )
    failed |= fail("the evaluation must not report an engine error");
  mt_close(runtime);

  if ( !failed ) puts("apostrophe boot path ok");
  return failed;
}

int main(void)
{ return test_engine_path_is_passed_as_data();
}
