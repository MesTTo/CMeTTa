/* Purpose: prove that the compiled boot IS the source boot, and that the two
 *   damaged trees a host meets still boot: one it may not write, and one
 *   carrying an artifact SWI cannot read.
 * Assumes: a scratch copy of the engine tree can be made under the checkout's
 *   ai-tmp/, `cp` is on PATH, and this binary can re-exec itself, because one
 *   process holds one Prolog runtime and each regime needs its own boot.
 * Guarantees: exits nonzero unless a read-only tree boots from source and
 *   writes nothing, a writable tree generates its artifacts, a boot through
 *   those artifacts exposes exactly the predicates the source boot exposed,
 *   and an unreadable engine/metta.qlf is recompiled rather than served.
 * Owns resources: the scratch tree, removed on every exit path, including the
 *   read-only directories, which are made writable again before the removal.
 * Fails when: run against an engine tree it may not copy, where it reports the
 *   copy rather than the engine.
 */

#define _POSIX_C_SOURCE 200809L
#define MT_SHORTHAND
#include <cmetta.h>
#include <SWI-Prolog.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { PATH_MAX_BYTES = 4096 };

static int failures;

static void expect(int condition, const char *claim)
{ if ( condition ) return;
  failures++;
  fprintf(stderr, "qlf boot regression failed: %s\nlast error: %s\n",
          claim, mt_errmsg() ? mt_errmsg() : "(none)");
}

/* Every non-imported predicate of every module, sorted, one per line. That is
   what "the same engine" means here: the .qlf regime installs its clauses
   through foreign code rather than by reading terms, so the question a
   reviewer has is whether the two routes end with the same predicates, not
   whether the loader printed the same thing on the way.

   The destination arrives in the environment rather than pasted into this
   text. A path is DATA, which is the rule goal_atom() states in cmetta.c
   after an apostrophe in a directory name once changed a boot goal, and a
   constant goal has no other way to read one. */
static const char DUMP_PREDICATES[] =
  "getenv('CMETTA_QLF_PREDICATES', File), "
  "findall(Module-Name/Arity, "
  "        ( current_predicate(_, Module:Head), "
  "          \\+ predicate_property(Module:Head, imported_from(_)), "
  "          functor(Head, Name, Arity) ), "
  "        Found), "
  "sort(Found, Sorted), "
  "setup_call_cleanup(open(File, write, Out), "
  "                   forall(member(P, Sorted), format(Out, '~q~n', [P])), "
  "                   close(Out))";

/* ------------------------------------------------------------------ *
 * The child: one boot, one regime
 * ------------------------------------------------------------------ */

static int boot_once(const char *tree)
{ mt_config config = { .path = tree };
  metta *runtime = mt_open(&config);

  if ( !runtime )
  { fprintf(stderr, "qlf boot: %s would not boot: %s\n",
            tree, mt_errmsg() ? mt_errmsg() : "(none)");
    return 1;
  }
  if ( mt_one_int(mt_run(runtime, "!(+ 1 2)\n")) != 3 || !mt_ok() )
  { fprintf(stderr, "qlf boot: %s booted but did not evaluate\n", tree);
    mt_close(runtime);
    return 1;
  }
  if ( getenv("CMETTA_QLF_PREDICATES") )
  { term_t goal = PL_new_term_ref();
    if ( !PL_chars_to_term(DUMP_PREDICATES, goal) || !PL_call(goal, NULL) )
    { fprintf(stderr, "qlf boot: the predicate listing did not run\n");
      mt_close(runtime);
      return 1;
    }
  }
  mt_close(runtime);
  return 0;
}

/* ------------------------------------------------------------------ *
 * The parent: the tree states, and a child per boot
 * ------------------------------------------------------------------ */

/* No shell anywhere in here: every argument is passed as its own vector
   element, so a checkout path holding a space or a quote is a path rather
   than more words. */
static int run(char *const argv[])
{ pid_t child = fork();
  int status;

  if ( child < 0 ) return -1;
  if ( child == 0 )
  { execvp(argv[0], argv);
    _exit(127);
  }
  if ( waitpid(child, &status, 0) != child ) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int boot_child(const char *self, const char *tree, const char *listing)
{ char *argv[] = { NULL, (char *)"--boot", NULL, NULL };
  argv[0] = (char *)self;
  argv[2] = (char *)tree;
  if ( listing )
    setenv("CMETTA_QLF_PREDICATES", listing, 1);
  else
    unsetenv("CMETTA_QLF_PREDICATES");
  return run(argv);
}

/* Refuses rather than truncates: a shortened path names a directory the caller
   did not mean, and a fixture that then deletes or chmods it is worse than a
   fixture that stops. */
static void join(char *out, const char *directory, const char *name)
{ if ( snprintf(out, PATH_MAX_BYTES, "%s/%s", directory, name) < PATH_MAX_BYTES )
    return;
  fprintf(stderr, "qlf boot: %s/%s is too long to be a path here\n",
          directory, name);
  exit(2);
}

/* The artifact set, which this test both counts and deletes. It is the
   engine's own set: the .qlf beside each engine umbrella, one more beside
   each library under lib/, and the stamp. */
static int walk_artifacts(const char *directory, int remove_them)
{ DIR *open_directory = opendir(directory);
  struct dirent *entry;
  int found = 0;

  if ( !open_directory ) return 0;
  while ( (entry = readdir(open_directory)) )
  { size_t length = strlen(entry->d_name);
    char path[PATH_MAX_BYTES];
    if ( length < 4 || strcmp(entry->d_name + length - 4, ".qlf") != 0 )
      continue;
    found++;
    if ( remove_them )
    { join(path, directory, entry->d_name);
      unlink(path);
    }
  }
  closedir(open_directory);
  return found;
}

/* lib/ holds one directory per library and one artifact inside each, so both
   the purge and the read-only arm have to reach a level deeper than engine/.
   The action is applied to lib/ itself as well, because that is where a mode
   change has to land for the library directories to be created or removed. */
static int walk_libraries(const char *tree, int remove_them, mode_t mode)
{ char libraries[PATH_MAX_BYTES];
  DIR *open_directory;
  struct dirent *entry;
  int found = 0;

  join(libraries, tree, "lib");
  if ( !(open_directory = opendir(libraries)) ) return 0;
  while ( (entry = readdir(open_directory)) )
  { char library[PATH_MAX_BYTES];
    struct stat details;
    if ( entry->d_name[0] == '.' ) continue;
    join(library, libraries, entry->d_name);
    if ( stat(library, &details) != 0 || !S_ISDIR(details.st_mode) ) continue;
    found += walk_artifacts(library, remove_them);
    if ( mode ) chmod(library, mode);
  }
  closedir(open_directory);
  if ( mode ) chmod(libraries, mode);
  return found;
}

static int artifacts(const char *tree, int remove_them, mode_t mode)
{ char engine[PATH_MAX_BYTES];
  char stamp[PATH_MAX_BYTES];
  int found;

  join(engine, tree, "engine");
  found = walk_artifacts(engine, remove_them);
  if ( remove_them )
  { join(stamp, engine, ".qlf-stamp");
    unlink(stamp);
  }
  found += walk_libraries(tree, remove_them, mode);
  if ( mode ) chmod(engine, mode);
  return found;
}

static long file_size(const char *path)
{ struct stat details;
  return stat(path, &details) == 0 ? (long)details.st_size : -1;
}

static int same_listing(const char *left, const char *right, long *lines)
{ FILE *a = fopen(left, "r");
  FILE *b = fopen(right, "r");
  int same = a && b;
  int one, two;

  *lines = 0;
  while ( same )
  { one = fgetc(a);
    two = fgetc(b);
    if ( one != two ) same = 0;
    else if ( one == EOF ) break;
    else if ( one == '\n' ) (*lines)++;
  }
  if ( a ) fclose(a);
  if ( b ) fclose(b);
  return same;
}

/* The engine tree this binary was built against, copied so that deleting an
   artifact, revoking a permission and damaging a .qlf never touch the
   checkout the gate is running in. `extensions` is LINKED rather than copied:
   the seats are what the engine globs at boot and one of them carries a
   multi-gigabyte Rust target directory, while nothing under extensions/ is a
   Quick Load Format artifact, so nothing there is written by a boot. */
static int build_tree(const char *source, const char *tree)
{ char member[PATH_MAX_BYTES];
  char destination[PATH_MAX_BYTES];
  char *copy[] = { (char *)"cp", (char *)"-R", NULL, NULL, NULL };
  const char *part;
  size_t i;

  if ( mkdir(tree, 0755) != 0 ) return 0;
  for (i = 0; i < 2; i++)
  { part = i == 0 ? "engine" : "lib";
    join(member, source, part);
    join(destination, tree, part);
    copy[2] = member;
    copy[3] = destination;
    if ( run(copy) != 0 ) return 0;
  }
  join(member, source, "extensions");
  join(destination, tree, "extensions");
  return symlink(member, destination) == 0;
}

/* The modes go back before the removal, because a case that failed midway may
   have left the read-only arm's 0555 on a directory rm would then refuse. */
static void remove_scratch(const char *scratch, const char *tree)
{ char *argv[] = { (char *)"rm", (char *)"-rf", NULL, NULL };
  argv[2] = (char *)scratch;
  artifacts(tree, 0, 0755);
  run(argv);
}

/* ------------------------------------------------------------------ *
 * The cases
 * ------------------------------------------------------------------ */

/* An installed engine tree is often not the running user's to write, and this
   is the arm that says what happens then: SWI compiles from source, saves
   nothing, and the host gets the engine anyway. It is the source arm of the
   comparison below as well, which is why it dumps a listing. */
static void test_a_read_only_engine_tree_boots_from_source(
    const char *self, const char *tree, const char *listing)
{ int booted;
  int left;

  artifacts(tree, 1, 0555);
  booted = boot_child(self, tree, listing);
  left = artifacts(tree, 0, 0755);
  expect(booted == 0, "a read-only engine tree must still boot");
  expect(left == 0, "a read-only engine tree must be left without artifacts");
}

/* The claim the whole change rests on. A .qlf load installs clauses through
   foreign code instead of reading terms, so the two routes are different
   machinery reaching the same place, and the only honest check is to ask the
   booted engine what it holds and compare the answers. */
static void test_the_compiled_boot_is_the_same_engine(
    const char *self, const char *tree, const char *from_source,
    const char *from_artifacts)
{ char umbrella[PATH_MAX_BYTES];
  char engine[PATH_MAX_BYTES];
  long lines = 0;

  join(engine, tree, "engine");
  join(umbrella, engine, "metta.qlf");
  expect(boot_child(self, tree, NULL) == 0,
         "a writable tree with no artifacts must boot and generate them");
  expect(file_size(umbrella) > 0,
         "that boot must leave engine/metta.qlf beside its source");
  expect(boot_child(self, tree, from_artifacts) == 0,
         "the next boot must load the artifacts it generated");
  expect(same_listing(from_source, from_artifacts, &lines),
         "the compiled boot must expose the source boot's predicates exactly");
  expect(lines > 1000,
         "the listing must hold the engine's predicates rather than nothing");
  printf("qlf boot: %ld predicates, identical from source and from .qlf\n",
         lines);
}

/* SWI reads the header first, so an artifact it cannot recognise is recompiled
   from source and republished, which is the recovery that actually exists.
   Larger tears are not recoverable and no lane can assert they are: 64 bytes
   and 4 kB abort the process from inside SWI's loader with `[FATAL ERROR:
   Unexpected EOF on QLF file]`, and a quarter, a half and three quarters hang
   it [measured 2026-09-05, eight damage sizes against engine/main.pl]. The
   artifact is published atomically through `.<name>.qlf.<pid>` and rename(2),
   so concurrent first boots do not produce any of them. */
static void test_an_unreadable_artifact_is_recompiled(
    const char *self, const char *tree, const char *expected,
    const char *after_repair)
{ char umbrella[PATH_MAX_BYTES];
  char engine[PATH_MAX_BYTES];
  long lines = 0;

  join(engine, tree, "engine");
  join(umbrella, engine, "metta.qlf");
  expect(truncate(umbrella, 0) == 0, "the artifact must be damageable");
  expect(boot_child(self, tree, NULL) == 0,
         "an unreadable engine/metta.qlf must not stop a boot");
  expect(file_size(umbrella) > 0,
         "that boot must republish the artifact it could not read");
  expect(boot_child(self, tree, after_repair) == 0,
         "the republished artifact must load on the next boot");
  expect(same_listing(expected, after_repair, &lines),
         "the republished artifact must hold the same engine");
}

int main(int argc, char **argv)
{ const char *source = getenv("METTA_PATH");
  char tree[PATH_MAX_BYTES];
  char root_scratch[PATH_MAX_BYTES];
  char scratch[PATH_MAX_BYTES];
  char from_source[PATH_MAX_BYTES];
  char from_artifacts[PATH_MAX_BYTES];
  char after_repair[PATH_MAX_BYTES];
  char run_name[64];

  if ( !source || !*source ) source = MT_ENGINE_PATH;
  if ( argc == 3 && strcmp(argv[1], "--boot") == 0 )
    return boot_once(argv[2]);
  if ( argc != 1 )
  { fprintf(stderr, "usage: test_qlf_boot [--boot <engine tree>]\n");
    return 2;
  }

  snprintf(run_name, sizeof(run_name), "cmetta-qlf-boot-%ld", (long)getpid());
  join(root_scratch, source, "ai-tmp");
  join(scratch, root_scratch, run_name);
  join(tree, scratch, "tree");
  join(from_source, scratch, "from-source.txt");
  join(from_artifacts, scratch, "from-artifacts.txt");
  join(after_repair, scratch, "after-repair.txt");
  mkdir(root_scratch, 0755);
  if ( mkdir(scratch, 0755) != 0 || !build_tree(source, tree) )
  { fprintf(stderr, "qlf boot: could not copy %s into %s\n", source, scratch);
    remove_scratch(scratch, tree);
    return 1;
  }

  test_a_read_only_engine_tree_boots_from_source(argv[0], tree, from_source);
  test_the_compiled_boot_is_the_same_engine(argv[0], tree, from_source,
                                            from_artifacts);
  test_an_unreadable_artifact_is_recompiled(argv[0], tree, from_artifacts,
                                            after_repair);

  remove_scratch(scratch, tree);
  if ( !failures ) puts("qlf boot regimes ok");
  return failures ? 1 : 0;
}
