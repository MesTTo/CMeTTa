# Purpose: build libcetta, the C binding, against the SWI-Prolog this box has.
# Assumes: swipl is on PATH and reports its own layout through
#   --dump-runtime-variables, which is how SWI tells a build where its headers
#   and libswipl live without a pkg-config file.
# Guarantees: `make` produces libcetta.so plus the examples; `make test` runs
#   the C suite and exits nonzero on the first failure.
# Decides: the engine tree is baked in as CETTA_ENGINE_PATH so a linked program
#   boots with no environment set, and $PETTA_PATH still overrides it at run
#   time. A checkout that moves needs a rebuild, which is the same bargain
#   setup.py makes when it copies the runtime into the Python wheel.

SWIPL       ?= swipl
PLBASE      := $(shell $(SWIPL) --dump-runtime-variables | sed -n 's/^PLBASE="\(.*\)";$$/\1/p')
PLLIBDIR    := $(shell $(SWIPL) --dump-runtime-variables | sed -n 's/^PLLIBDIR="\(.*\)";$$/\1/p')
ENGINE_PATH ?= $(abspath $(CURDIR)/../..)

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -fPIC -I. -I$(PLBASE)/include \
           -DCETTA_ENGINE_PATH='"$(ENGINE_PATH)"'
LDFLAGS += -L$(PLLIBDIR) -Wl,-rpath,$(PLLIBDIR)
LDLIBS  += -lswipl

LIB       := libcetta.so
EXAMPLES  := examples/hello examples/ops examples/stream
TESTS     := tests/test_cetta
KIT       := kit/driver

.PHONY: all test examples kit clean

kit: $(KIT)

all: $(LIB) examples $(KIT)

$(LIB): cetta.c cetta.h
	$(CC) $(CFLAGS) -shared -o $@ cetta.c $(LDFLAGS) $(LDLIBS)

examples: $(EXAMPLES)

examples/%: examples/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcetta $(LDFLAGS) $(LDLIBS) -lm

kit/%: kit/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcetta $(LDFLAGS) $(LDLIBS) -lm

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcetta $(LDFLAGS) $(LDLIBS) -lm

test: $(TESTS)
	@./tests/test_cetta

clean:
	rm -f $(LIB) $(EXAMPLES) $(TESTS) $(KIT)
