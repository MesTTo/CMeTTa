# Purpose: build libcetta, the C binding, against the SWI-Prolog this box has.
# Assumes: swipl is on PATH and reports its own layout through
#   --dump-runtime-variables, which is how SWI tells a build where its headers
#   and libswipl live without a pkg-config file. That assumption is CHECKED
#   below rather than described here, because prose in a comment cannot refuse
#   anything and this one had three sentences nothing read.
#   swipl-ld is not the tool for this seat: it builds an extension loaded INTO
#   SWI, and this one goes the other way, calling PL_initialise to embed SWI in
#   a C program.
# Guarantees: `make` produces libcetta.so plus the examples; `make test` runs
#   the C suite and exits nonzero on the first failure; a missing prerequisite
#   stops the build NAMING it and the package that supplies it, where an empty
#   PLBASE used to compile against -I/include and fail on a missing
#   SWI-Prolog.h. `make clean` needs no toolchain and is exempt, so a machine
#   that cannot build this can still tidy up after one that could.
# Decides: the engine tree is baked in as MT_ENGINE_PATH so a linked program
#   boots with no environment set, and $METTA_PATH still overrides it at run
#   time. A checkout that moves needs a rebuild, which is the same bargain
#   setup.py makes when it copies the runtime into the Python wheel.

SWIPL       ?= swipl
PLBASE      := $(shell $(SWIPL) --dump-runtime-variables 2>/dev/null | sed -n 's/^PLBASE="\(.*\)";$$/\1/p')
PLLIBDIR    := $(shell $(SWIPL) --dump-runtime-variables 2>/dev/null | sed -n 's/^PLLIBDIR="\(.*\)";$$/\1/p')
ENGINE_PATH ?= $(abspath $(CURDIR)/../..)

# The prerequisites, checked rather than described. They were three sentences in
# the header above and nothing read them, so a tree without SWI's development
# files got PLBASE="" and compiled against -I/include, failing on a missing
# SWI-Prolog.h: the true cause named nowhere. `clean` is exempt because removing
# build products needs no toolchain, and a component must be able to tidy up on
# a machine that cannot build it.
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(PLBASE),)
$(error swipl is not on PATH or does not answer --dump-runtime-variables; \
        this binding EMBEDS SWI-Prolog and needs its development files. \
        Set SWIPL=/path/to/swipl, or install the SWI-Prolog development package)
endif
ifeq ($(wildcard $(PLBASE)/include/SWI-Prolog.h),)
$(error SWI-Prolog.h is absent under $(PLBASE)/include; swipl is installed but \
        its development headers are not. Install the SWI-Prolog development package)
endif
ifeq ($(wildcard $(PLLIBDIR)/libswipl*),)
$(error libswipl is absent under $(PLLIBDIR); this binding links it directly, \
        which is what EMBEDS the engine in a C program)
endif
endif

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -fPIC -I. -I$(PLBASE)/include \
           -DMT_ENGINE_PATH='"$(ENGINE_PATH)"'
LDFLAGS += -L$(PLLIBDIR) -Wl,-rpath,$(PLLIBDIR)
LDLIBS  += -lswipl

LIB       := libcetta.so
EXAMPLES  := examples/hello examples/ops examples/stream
TESTS     := tests/test_cetta
KIT       := kit/driver
BENCH     := benchmarks/cases

.PHONY: all test bench examples kit clean

kit: $(KIT)

# The benchmark driver is a target of its own so bench.sh can ask for it
# without rebuilding the suite, and so `make all` still produces everything a
# fresh checkout needs.
bench: $(BENCH)

all: $(LIB) examples $(KIT) $(BENCH)

$(LIB): cetta.c cetta.h
	$(CC) $(CFLAGS) -shared -o $@ cetta.c $(LDFLAGS) $(LDLIBS)

examples: $(EXAMPLES)

examples/%: examples/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcetta $(LDFLAGS) $(LDLIBS) -lm

kit/%: kit/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcetta $(LDFLAGS) $(LDLIBS) -lm

benchmarks/%: benchmarks/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcetta $(LDFLAGS) $(LDLIBS) -lm

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcetta $(LDFLAGS) $(LDLIBS) -lm

# The examples run too. An example that no longer compiles, or that compiles
# and then fails, is documentation that lies, and the README quotes these
# three directly. The Python seat gates its examples for the same reason.
test: $(TESTS) $(EXAMPLES)
	@./tests/test_cetta
	@for example in $(EXAMPLES); do \
	    ./$$example > /dev/null || { echo "$$example failed" >&2; exit 1; }; \
	    echo "$$example ok"; \
	done

clean:
	rm -f $(LIB) $(EXAMPLES) $(TESTS) $(KIT) $(BENCH)
