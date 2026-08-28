/* Purpose: run ONE named C-host workload for a fixed number of operations and
 *   report what the engine spent on it, so extensions/cetta/benchmarks/bench.py
 *   can pin it. Every case here is a door a C host actually goes through, and
 *   each says beside itself which counter decides it.
 *
 * Assumes:
 *   - argv is `cases <name> <iterations> [--controlled]`; nothing else is read
 *   - with --controlled, perf's control descriptors arrive in
 *     METTA_PERF_CONTROL_FD, METTA_PERF_ACK_FD and METTA_PERF_CLOSE_FDS, the
 *     protocol metta.benchmarking._run_perf speaks and benchmarks/pure.py's
 *     _controlled answers on the Python side
 *
 * Guarantees:
 *   - setup and teardown sit OUTSIDE the counted region, so a per-operation
 *     case measures the operation and not the engine boot in front of it.
 *     perf's own manual gives the reason for the mechanism: --delay=-1 starts
 *     with events disabled, "useful to filter out the startup phase of the
 *     program, which is often very different" [source: perf-stat(1),
 *     --control=fd and -D]
 *   - every case checks the work it did and exits 1 naming the failure, so a
 *     workload that quietly stopped doing anything cannot report a fast time.
 *     A benchmark whose result nothing reads is a benchmark the optimiser and
 *     the engine are both free to skip
 *   - stdout carries `case`, `operations`, `inferences` and `checksum`, one
 *     per line. `inferences` is the engine's own counter across the counted
 *     region, which is BLIND to everything this binding does in C: foreign
 *     code retires no inferences at all. It is reported for the cases whose
 *     work is mostly engine-side, and it never decides one on its own
 *
 * Owns resources: one runtime, closed before returning; every atom, cursor and
 *   text this file builds is released on the path that built it.
 *
 * Decides: one process per case. cetta_open() is once per process by
 *   construction (PL_initialise sets up the process's single Prolog heap), so
 *   a second case in the same process would measure a warmed engine while the
 *   first measured a cold one.
 *
 * Open Obligations:
 *   To Do: None
 *   Hacks: None
 *   Future Enhancements: None
 */

#include <cetta.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read by main() and printed, so no case's work can be removed as dead. */
static volatile unsigned long long sink;

static int fail(const char *what)
{ fprintf(stderr, "cases: %s: %s\n", what,
          cetta_errmsg() ? cetta_errmsg() : "(no error text)");
  return 1;
}

/* ------------------------------------------------------------------ *
 * perf's control window
 * ------------------------------------------------------------------ */

/* perf writes `ack\n` and pads to its own buffer width, so the reply is read
   until a newline arrives rather than by an exact length. */
static int wait_for_ack(int descriptor)
{ char reply[16] = {0};
  size_t filled = 0;
  while ( filled < sizeof(reply) && memchr(reply, '\n', filled) == NULL )
  { ssize_t got = read(descriptor, reply + filled, sizeof(reply) - filled);
    if ( got <= 0 )
    { fprintf(stderr, "cases: perf control acknowledgement pipe closed\n");
      return 1;
    }
    filled += (size_t)got;
  }
  if ( strncmp(reply, "ack\n", 4) != 0 )
  { fprintf(stderr, "cases: invalid perf control acknowledgement\n");
    return 1;
  }
  return 0;
}

static int control_fd = -1;
static int acknowledge_fd = -1;

/* Take the descriptors perf handed down and close the two ends that belong to
   perf itself, which the Python side named for us. The comma-separated list is
   walked in place rather than through strtok on a strdup: strdup is POSIX and
   this file is built -std=c11, where glibc hides it. */
static int control_open(void)
{ const char *control = getenv("METTA_PERF_CONTROL_FD");
  const char *acknowledge = getenv("METTA_PERF_ACK_FD");
  const char *closing = getenv("METTA_PERF_CLOSE_FDS");
  const char *scan;

  if ( !control || !acknowledge || !closing )
  { fprintf(stderr, "cases: --controlled needs perf's control descriptors; "
                    "run this through metta.benchmarking.measure_counters\n");
    return 1;
  }
  for (scan = closing; *scan; )
  { close(atoi(scan));
    while ( *scan && *scan != ',' ) scan++;
    if ( *scan == ',' ) scan++;
  }
  control_fd = atoi(control);
  acknowledge_fd = atoi(acknowledge);
  return 0;
}

static int control_send(const char *command)
{ size_t length;
  if ( control_fd < 0 ) return 0;
  length = strlen(command);
  if ( write(control_fd, command, length) != (ssize_t)length )
  { fprintf(stderr, "cases: perf control command was truncated\n");
    return 1;
  }
  return wait_for_ack(acknowledge_fd);
}

/* ------------------------------------------------------------------ *
 * The cases
 * ------------------------------------------------------------------ */

/* What one case needs between setup and teardown. */
typedef struct workload
{ cetta_t         *m;
  cetta_space_t   *space;
  cetta_answers_t *cursor;
  cetta_atom_t    *atom;
  cetta_atom_t   **facts;
  cetta_atom_t   **patterns;
  const char      *source;
  size_t           n;
  /* cetta_self() is borrowed and cetta_space_open() is owned, so teardown has
     to be told which one it holds. */
  bool             owns_space;
} workload_t;

/* A term with one of most kinds in it, so a crossing measures the encoder and
   the decoder across their branches rather than across one. */
static cetta_atom_t *sample_term(void)
{ return cetta_expr(4,
           cetta_sym("edge"),
           cetta_expr(3, cetta_sym("node"), cetta_sym("a"), cetta_int(1)),
           cetta_expr(3, cetta_sym("node"), cetta_sym("b"), cetta_float(2.5)),
           cetta_expr(2, cetta_sym("label"), cetta_str("x")));
}

static const char SAMPLE_SOURCE[] =
  "(edge (node a 1) (node b 2.5) (label \"x\"))";

/* boot: what a C host pays before it can ask the engine anything, measured as
   the WHOLE process because that is what it costs -- the dynamic loader,
   PL_initialise, and consulting the engine, none of which a control window
   opened inside main() would see. Decided by instructions:u and CPU time
   together: almost none of this is engine reduction, so the inference counter
   sees a small fraction of it and cannot referee the rest. The inference count
   is still reported, because consulting the engine is the part it CAN see.
   The proof of "usable" rather than merely "open" is one evaluated form. */
static int case_boot(workload_t *w)
{ cetta_answers_t *answers = NULL;
  int64_t value = 0;

  if ( cetta_run(w->m, "!(+ 1 2)\n", &answers) != CETTA_OK )
    return fail("boot: the engine booted but could not evaluate");
  if ( cetta_answers_step(answers) != CETTA_ROW ||
       cetta_int_value(cetta_answers_atom(answers), &value) != CETTA_OK ||
       value != 3 )
  { cetta_answers_free(answers);
    fprintf(stderr, "cases: boot: the engine answered something other than 3\n");
    return 1;
  }
  cetta_answers_free(answers);
  sink += (unsigned long long)value;
  return 0;
}

/* cursor-step: one metta_c_next per step, which is the door a C host pulls an
   answer through. Mostly ENGINE work driven from C -- the engine computes one
   answer, then this binding decodes it into a cetta_atom_t and renders its
   text -- so the inference count is meaningful here and is pinned. It still
   does not decide: the decode and the text are C, and the inference counter
   cannot see either. instructions:u and CPU time decide; inferences catch a
   change in what the engine did per answer. */
static int case_cursor_step(workload_t *w)
{ size_t i;
  for (i = 0; i < w->n; i++)
  { int64_t value = 0;
    if ( cetta_answers_step(w->cursor) != CETTA_ROW )
      return fail("cursor-step: the generator stopped answering");
    if ( cetta_int_value(cetta_answers_atom(w->cursor), &value) != CETTA_OK )
      return fail("cursor-step: an answer was not the integer the generator yields");
    sink += (unsigned long long)value;
  }
  return 0;
}

/* term-in: a term crossing FROM C INTO the engine. cetta_show encodes a C atom
   into a Prolog term and asks the engine to write it, which is the only public
   door that crosses in this direction without also storing or evaluating
   something. Decided by instructions:u and CPU time: the encode is pure C and
   retires no inferences, and the writer on the other side is what the
   inference count would see. */
static int case_term_in(workload_t *w)
{ size_t i;
  for (i = 0; i < w->n; i++)
  { char *text = cetta_show(w->m, w->atom);
    if ( !text ) return fail("term-in: cetta_show refused the term");
    sink += (unsigned char)text[0];
    cetta_free(text);
  }
  return 0;
}

/* term-out: a term crossing FROM the engine OUT to C. cetta_parse runs the
   engine's reader and then decodes the resulting Prolog term into a
   cetta_atom_t, which is this direction's mirror of term-in: same term, same
   engine text door, opposite crossing. Decided by the same pair and for the
   same reason -- decode is C and retires nothing. */
static int case_term_out(workload_t *w)
{ size_t i;
  for (i = 0; i < w->n; i++)
  { cetta_atom_t *atom = NULL;
    if ( cetta_parse(w->m, w->source, &atom) != CETTA_OK || !atom )
      return fail("term-out: cetta_parse refused the source");
    sink += cetta_len(atom);
    cetta_release(atom);
  }
  return 0;
}

/* space-pair: store one fact and retrieve it, which is what a C host does with
   a space. The atoms are built OUTSIDE the region so the row prices the two
   space doors rather than C-side construction, and the pattern names the key
   just added so the retrieval is a lookup rather than a scan. Mostly engine
   work: the add asserts and the match runs the engine's own matcher, so the
   inference count is pinned here too, beside the deciding pair. */
static int case_space_pair(workload_t *w)
{ size_t i;
  for (i = 0; i < w->n; i++)
  { cetta_answers_t *answers = NULL;
    int64_t value = 0;
    if ( cetta_add(w->space, w->facts[i]) != CETTA_OK )
      return fail("space-pair: cetta_add refused a fact");
    if ( cetta_match(w->space, w->patterns[i], &answers) != CETTA_OK )
      return fail("space-pair: cetta_match refused a pattern");
    if ( cetta_answers_step(answers) != CETTA_ROW )
    { cetta_answers_free(answers);
      return fail("space-pair: the fact just added did not match");
    }
    if ( cetta_int_value(cetta_child(cetta_answers_atom(answers), 2), &value)
         != CETTA_OK )
    { cetta_answers_free(answers);
      return fail("space-pair: the matched fact carried no integer");
    }
    sink += (unsigned long long)value;
    cetta_answers_free(answers);
  }
  return 0;
}

/* error-ball: an engine exception crossing back to C as words. The engine
   raises, call_bridge copies the ball off the stacks with PL_record, and
   render_ball asks metta_c_error_text/2 for its text -- three things nothing
   else in this suite does, and the path every C host meets the first time a
   program of its own is wrong. A failed assertion is the raiser because MeTTa
   keeps most failures AS values: (car-atom 5) answers unit and (+ 1 foo)
   answers itself, so neither reaches this path at all
   [source: extensions/cetta/tests/test_cetta.c, test_an_engine_error_reaches_c_as_words].
   Decided by instructions:u and CPU time; no inference pin, because the ball's
   rendering is a foreign-side round trip whose engine half is a fraction of
   the row. */
static int case_error_ball(workload_t *w)
{ size_t i;
  for (i = 0; i < w->n; i++)
  { cetta_answers_t *answers = NULL;
    const char *said;
    if ( cetta_eval(w->space, w->atom, &answers) != CETTA_OK )
    { cetta_answers_free(answers);
      return fail("error-ball: the goal could not be opened");
    }
    if ( cetta_answers_step(answers) != CETTA_ERROR )
    { cetta_answers_free(answers);
      fprintf(stderr, "cases: error-ball: the goal did not raise\n");
      return 1;
    }
    said = cetta_errmsg();
    if ( !said || !*said )
    { cetta_answers_free(answers);
      fprintf(stderr, "cases: error-ball: the ball rendered to nothing\n");
      return 1;
    }
    sink += (unsigned char)said[0];
    cetta_answers_free(answers);
  }
  return 0;
}

/* ------------------------------------------------------------------ *
 * Setup and teardown, both outside the counted region
 * ------------------------------------------------------------------ */

static int setup_cursor_step(workload_t *w)
{ cetta_answers_t *answers = NULL;
  cetta_atom_t *goal;
  if ( cetta_run(w->m, "(= (from $n) (superpose ($n (from (+ $n 1)))))\n",
                 &answers) != CETTA_OK )
    return fail("cursor-step: the generator could not be defined");
  cetta_answers_free(answers);
  if ( !(goal = cetta_expr(2, cetta_sym("from"), cetta_int(0))) )
    return fail("cursor-step: the goal could not be built");
  w->atom = goal;
  if ( cetta_eval(cetta_self(w->m), goal, &w->cursor) != CETTA_OK )
    return fail("cursor-step: the cursor could not be opened");
  return 0;
}

static int setup_space_pair(workload_t *w)
{ size_t i;
  if ( cetta_space_open(w->m, "&cetta-bench", &w->space) != CETTA_OK )
    return fail("space-pair: the space could not be opened");
  w->owns_space = true;
  w->facts = calloc(w->n, sizeof(*w->facts));
  w->patterns = calloc(w->n, sizeof(*w->patterns));
  if ( !w->facts || !w->patterns )
  { fprintf(stderr, "cases: space-pair: out of memory building the fixture\n");
    return 1;
  }
  for (i = 0; i < w->n; i++)
  { w->facts[i] = cetta_expr(3, cetta_sym("fact"), cetta_int((int64_t)i),
                             cetta_int((int64_t)i * 2));
    w->patterns[i] = cetta_expr(3, cetta_sym("fact"), cetta_int((int64_t)i),
                                cetta_var("v"));
    if ( !w->facts[i] || !w->patterns[i] )
      return fail("space-pair: a fixture atom could not be built");
  }
  return 0;
}

static int setup_error_ball(workload_t *w)
{ w->space = cetta_self(w->m);
  if ( !(w->atom = cetta_expr(3, cetta_sym("assertEqual"), cetta_int(1),
                              cetta_int(2))) )
    return fail("error-ball: the goal could not be built");
  return 0;
}

static void teardown(workload_t *w)
{ size_t i;
  cetta_answers_free(w->cursor);
  cetta_release(w->atom);
  if ( w->facts || w->patterns )
    for (i = 0; i < w->n; i++)
    { if ( w->facts ) cetta_release(w->facts[i]);
      if ( w->patterns ) cetta_release(w->patterns[i]);
    }
  free(w->facts);
  free(w->patterns);
  if ( w->owns_space ) cetta_space_free(w->space);
}

/* ------------------------------------------------------------------ *
 * The table
 * ------------------------------------------------------------------ */

typedef struct bench_case
{ const char *name;
  int       (*setup)(workload_t *w);
  int       (*run)(workload_t *w);
  /* boot is the process, so it cannot be measured inside a control window and
     runs exactly once. */
  bool        whole_process;
} bench_case_t;

static const bench_case_t CASES[] = {
  { "boot",        NULL,              case_boot,        true  },
  { "cursor-step", setup_cursor_step, case_cursor_step, false },
  { "term-in",     NULL,              case_term_in,     false },
  { "term-out",    NULL,              case_term_out,    false },
  { "space-pair",  setup_space_pair,  case_space_pair,  false },
  { "error-ball",  setup_error_ball,  case_error_ball,  false },
};

static const bench_case_t *find_case(const char *name)
{ size_t i;
  for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++)
    if ( strcmp(CASES[i].name, name) == 0 ) return &CASES[i];
  return NULL;
}

static void usage(void)
{ size_t i;
  fprintf(stderr, "usage: cases <case> <iterations> [--controlled]\ncases:");
  for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++)
    fprintf(stderr, " %s", CASES[i].name);
  fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{ const bench_case_t *chosen;
  workload_t w;
  cetta_stats_t before, after, spent;
  long iterations;
  bool controlled = false;
  int status = 0;

  memset(&w, 0, sizeof(w));
  memset(&before, 0, sizeof(before));
  if ( argc < 3 ) { usage(); return 2; }
  if ( !(chosen = find_case(argv[1])) ) { usage(); return 2; }
  iterations = strtol(argv[2], NULL, 10);
  if ( iterations < 1 ) { usage(); return 2; }
  if ( argc > 3 )
  { if ( strcmp(argv[3], "--controlled") != 0 ) { usage(); return 2; }
    controlled = true;
  }
  if ( chosen->whole_process )
  { if ( controlled )
    { fprintf(stderr, "cases: %s measures the whole process and cannot run "
                      "inside a control window\n", chosen->name);
      return 2;
    }
    if ( iterations != 1 )
    { fprintf(stderr, "cases: %s runs exactly once\n", chosen->name);
      return 2;
    }
  }
  w.n = (size_t)iterations;
  w.source = SAMPLE_SOURCE;
  if ( controlled && control_open() != 0 ) return 2;

  if ( cetta_open(NULL, &w.m) != CETTA_OK ) return fail("boot");
  /* term-in and term-out share one fixture and neither needs the engine to
     build it, which is why it is here rather than behind a setup hook. */
  if ( !chosen->setup && !chosen->whole_process && !(w.atom = sample_term()) )
    return fail("the sample term could not be built");
  if ( chosen->setup && chosen->setup(&w) != 0 ) { teardown(&w); return 1; }

  /* boot's counted region begins before this process had an engine to sample,
     so its `before` stays the zeroed struct and the delta reports what the
     engine retired from process start. Every other case samples the pair
     around its own region. */
  if ( !chosen->whole_process && cetta_stats(w.m, &before) != CETTA_OK )
    return fail("stats");
  if ( control_send("enable\n") != 0 ) { teardown(&w); return 2; }
  status = chosen->run(&w);
  if ( control_send("disable\n") != 0 ) { teardown(&w); return 2; }
  if ( cetta_stats(w.m, &after) != CETTA_OK ) return fail("stats");
  cetta_stats_delta(&before, &after, &spent);

  teardown(&w);
  if ( status == 0 )
  { printf("case %s\n", chosen->name);
    printf("operations %zu\n", w.n);
    printf("inferences %llu\n", (unsigned long long)spent.inferences);
    printf("checksum %llu\n", (unsigned long long)sink);
  }
  cetta_close(w.m);
  return status;
}
