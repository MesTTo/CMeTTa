/* Purpose: prove the stack ceiling a runtime boots under, and the one mt_limit
 *   restores, are the ones the host asked for: mt_config.stack_limit, else
 *   METTA_STACK_LIMIT, else MT_STACK_LIMIT_DEFAULT, the default settings.h
 *   carries from the Python seat's declaration, so the two seats boot under
 *   one number.
 * Assumes: built against the fault library, whose mt_test_stack_limit()
 *   reads SWI's stack_limit flag; the engine tree is MT_ENGINE_PATH.
 * Guarantees: exits nonzero, naming the case, when a ceiling differs from the
 *   one asked for, when clearing a bound restores anything but the boot
 *   ceiling, or when a METTA_STACK_LIMIT that is not a positive decimal
 *   integer boots instead of refusing with MT_MISUSE and the Python seat's
 *   words.
 * Owns resources: one runtime per forked child, closed before it exits; the
 *   parent never opens one, so every child starts from an uninitialised SWI.
 */

/* setenv, unsetenv and fork are POSIX, which -std=c11 leaves undeclared. */
#define _POSIX_C_SOURCE 200809L
#define MT_SHORTHAND
#include <cmetta.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern size_t mt_test_stack_limit(void);

/* One boot: what METTA_STACK_LIMIT holds, NULL unsetting it, and what the
   host configures, 0 configuring nothing. The boot either runs under `want`
   or, when `words` is set, refuses saying them. */
typedef struct ceiling_case {
  const char *name;
  const char *environment;
  size_t      configured;
  size_t      want;
  const char *words;
} ceiling_case;

#define REFUSED(value) MT_STACK_LIMIT_ENVIRONMENT " must be a positive integer, got '" value "'"

static const ceiling_case cases[] = {
  { "no host value", NULL, 0, (size_t)MT_STACK_LIMIT_DEFAULT, NULL },
  { "the environment", "3000000000", 0, 3000000000u, NULL },
  { "the host over the environment", "3000000000", 2500000000u, 2500000000u, NULL },
  { "words", "eight gigabytes", 0, 0, REFUSED("eight gigabytes") },
  { "empty", "", 0, 0, REFUSED("") },
  { "a sign", "+3000000000", 0, 0, REFUSED("+3000000000") },
  { "wider than size_t", "99999999999999999999999", 0, 0, REFUSED("99999999999999999999999") },
  { "zero", "0", 0, 0, MT_STACK_LIMIT_ENVIRONMENT " must be positive, got 0" },
};

static int report(const ceiling_case *c, int ok, const char *why)
{ if ( ok ) return 0;
  fprintf(stderr, "stack ceiling case '%s' failed: %s\nlast error: %s\n",
          c->name, why, mt_errmsg() ? mt_errmsg() : "(none)");
  return 1;
}

/* The boot runs under `want`, and clearing a narrower bound restores it
   rather than SWI's own default; or it refuses in the Python seat's words. */
static int boot(const ceiling_case *c)
{ metta *m;
  int failed;

  if ( c->environment ) setenv(MT_STACK_LIMIT_ENVIRONMENT, c->environment, 1);
  else unsetenv(MT_STACK_LIMIT_ENVIRONMENT);
  mt_clear();
  m = mt_open(&(mt_config){ .stack_limit = c->configured });
  if ( c->words )
  { failed = report(c, m == NULL, "a bad value booted");
    failed |= report(c, mt_error() == MT_MISUSE, "the refusal is not MT_MISUSE");
    failed |= report(c, mt_errmsg() && strstr(mt_errmsg(), c->words) != NULL,
                     "the refusal does not say why in the Python seat's words");
  } else
  { failed = report(c, m != NULL, "the boot was refused");
    if ( m )
    { failed |= report(c, mt_test_stack_limit() == c->want,
                       "the booted ceiling is not the one asked for");
      failed |= report(c, mt_limit(m, (mt_limits){ .stack_bytes = c->want / 2 }) &&
                          mt_test_stack_limit() == c->want / 2,
                       "a narrower bound did not reach SWI");
      failed |= report(c, mt_limit(m, (mt_limits){0}) && mt_test_stack_limit() == c->want,
                       "clearing the bound did not restore the booted ceiling");
    }
  }
  if ( m ) mt_close(m);
  return failed;
}

/* Each case in a process of its own, because a process holds one runtime and
   the case is what the boot does. Time: one engine boot per case. */
int main(void)
{ size_t count = sizeof cases / sizeof *cases;
  int failed = 0;

  for (size_t i = 0; i < count; i++)
  { pid_t child = fork();
    int status;

    if ( child < 0 )
    { perror("fork");
      return 1;
    }
    if ( child == 0 ) _exit(boot(&cases[i]));
    if ( waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 )
    { fprintf(stderr, "stack ceiling case '%s' did not pass\n", cases[i].name);
      failed = 1;
    }
  }
  if ( !failed ) printf("stack ceiling ok: %zu boots\n", count);
  return failed;
}
