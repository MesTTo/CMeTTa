# Purpose: build libcmetta, the C binding, against the SWI-Prolog this box has.
# Assumes: swipl is on PATH and reports its own layout through
#   --dump-runtime-variables, which is how SWI tells a build where its headers
#   and libswipl live without a pkg-config file. That assumption is CHECKED
#   below rather than described here, because prose in a comment cannot refuse
#   anything and this one had three sentences nothing read.
#   swipl-ld is not the tool for this seat: it builds an extension loaded INTO
#   SWI, and this one goes the other way, calling PL_initialise to embed SWI in
#   a C program.
# Guarantees: `make` produces libcmetta.so plus the examples; `make test` runs
#   the C suite and exits nonzero on the first failure; a missing prerequisite
#   stops the build NAMING it and the package that supplies it, where an empty
#   PLBASE used to compile against -I/include and fail on a missing
#   SWI-Prolog.h. `make clean` needs no toolchain and is exempt, so a machine
#   that cannot build this can still tidy up after one that could.
#   `make install` puts a versioned library, its soname links, the header, a
#   pkg-config file and the engine tree under $PREFIX, and `make install-check`
#   proves the result by compiling a consumer that knows only what pkg-config
#   says and booting it with no METTA_PATH and no rpath into this checkout
#   [tested: extensions/cmetta/check.sh c-install; commit=1c40a5f96c308941b4c0669594acb06403109751].
# Decides: the engine tree is baked in as MT_ENGINE_PATH so a linked program
#   boots with no environment set, and $METTA_PATH still overrides it at run
#   time. A checkout that moves needs a rebuild, which is the same bargain
#   setup.py makes when it copies the runtime into the Python wheel.
#   Every ordinary build treats warnings as errors, refuses undefined shared-
#   library symbols, and emits stack-protected full-RELRO objects. `make
#   sanitize` rebuilds in root ai-tmp so sanitizer and ordinary objects never
#   contaminate one another [tested: GATE_ONLY=1 sh check.sh c-sanitize;
#   commit=b339084bb5625996fc88a31608d48ad31c575d1f].

SWIPL       ?= swipl
PLBASE      := $(shell $(SWIPL) --dump-runtime-variables 2>/dev/null | sed -n 's/^PLBASE="\(.*\)";$$/\1/p')
PLLIBDIR    := $(shell $(SWIPL) --dump-runtime-variables 2>/dev/null | sed -n 's/^PLLIBDIR="\(.*\)";$$/\1/p')
ENGINE_PATH ?= $(abspath $(CURDIR)/../..)
TEST_TMP    := $(CURDIR)/ai-tmp/cmetta-parity

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
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Werror -fPIC \
           -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
           -I. -I$(PLBASE)/include -DMT_ENGINE_PATH='"$(ENGINE_PATH)"'
LDFLAGS += -L$(PLLIBDIR) -Wl,-rpath,$(PLLIBDIR) -Wl,-z,defs \
           -Wl,-z,relro,-z,now
LDLIBS  += -lswipl

LIB       := libcmetta.so
FAULT_LIB := tests/libcmetta_fault.so
EXAMPLES  := examples/hello examples/ops examples/stream examples/lower examples/language
FAULT_TESTS := tests/test_alloc_failure tests/test_cursor_ids tests/test_reopen \
               tests/test_internal_contracts tests/test_hash
THREAD_TESTS := tests/test_threads
TESTS     := tests/test_cmetta tests/test_bad_boot tests/test_quoted_path \
             tests/test_qlf_boot tests/test_batch_add tests/test_unify \
             tests/test_seam tests/test_ownership tests/test_iterators tests/test_transactions tests/test_providers tests/test_native_parity \
             tests/test_subscriptions tests/test_extensions \
             $(FAULT_TESTS) $(THREAD_TESTS)
KIT       := kit/driver
BENCH     := benchmarks/cases

# WHERE AN INSTALL PUTS THINGS, in the GNU spelling every packager already
# knows: PREFIX chooses the tree, DESTDIR stages it somewhere else so a package
# build never writes outside its own sandbox, and each directory can be moved
# on its own. Nothing here is read by the in-tree build; `make` and `make test`
# behave exactly as before.
PREFIX       ?= /usr/local
DESTDIR      ?=
libdir       ?= $(PREFIX)/lib
includedir   ?= $(PREFIX)/include
datadir      ?= $(PREFIX)/share
pkgconfigdir ?= $(libdir)/pkgconfig
# The engine tree the installed library boots. It has to be installed too: this
# library EMBEDS a Prolog engine and consults MeTTa source at run time, so a
# copy of libcmetta.so with no engine beside it can do nothing at all. $METTA_PATH
# still overrides it, which is what lets a developer point an installed library
# at a checkout.
enginedir    ?= $(datadir)/metta

# The soname carries the MAJOR version alone, so a consumer linked against
# libcmetta.so.1 keeps working across compatible releases and stops linking
# when the surface breaks. Two files and two symlinks is the layout every
# ELF toolchain expects. VERSION is derived from the public header and
# `make version` checks the loaded library against it.
VERSION   := $(shell sed -n 's/^\#define MT_VERSION "\([^"]*\)"/\1/p' cmetta.h)
SOVERSION := $(firstword $(subst ., ,$(VERSION)))
SOFILE    := libcmetta.so.$(VERSION)
SONAME    := libcmetta.so.$(SOVERSION)

.PHONY: all test bench examples kit surface docs version hardening sanitize runtime-memory \
        install uninstall install-check clean FORCE

kit: $(KIT)

# The benchmark driver is a target of its own so bench.sh can ask for it
# without rebuilding the suite, and so `make all` still produces everything a
# fresh checkout needs.
bench: $(BENCH)

all: $(LIB) examples $(KIT) $(BENCH)

$(LIB): cmetta.c cmetta.h
	$(CC) $(CFLAGS) -shared -o $@ cmetta.c $(LDFLAGS) $(LDLIBS)

$(FAULT_LIB): cmetta.c cmetta.h
	$(CC) $(CFLAGS) -DMT_TEST_FAULTS -shared -o $@ cmetta.c \
	    $(LDFLAGS) $(LDLIBS)

examples: $(EXAMPLES)

examples/%: examples/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcmetta $(LDFLAGS) $(LDLIBS) -lm

kit/%: kit/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcmetta $(LDFLAGS) $(LDLIBS) -lm

benchmarks/%: benchmarks/%.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcmetta $(LDFLAGS) $(LDLIBS) -lm

tests/%: tests/%.c tests/allocation_tracker.h $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcmetta $(LDFLAGS) $(LDLIBS) -lm

tests/extension_accept.so: tests/extension_fixture.c $(LIB)
	$(CC) $(CFLAGS) -shared -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcmetta $(LDFLAGS)

tests/extension_refuse.so: tests/extension_fixture.c $(LIB)
	$(CC) $(CFLAGS) -DCMETTA_FIXTURE_REFUSE -shared -o $@ $< -L. -Wl,-rpath,$(CURDIR) -lcmetta $(LDFLAGS)

tests/test_extensions: tests/extension_accept.so tests/extension_refuse.so

$(FAULT_TESTS): %: %.c $(FAULT_LIB)
	$(CC) $(CFLAGS) -DMT_TEST_FAULTS -o $@ $< -Ltests \
	    -Wl,-rpath,$(CURDIR)/tests -lcmetta_fault $(LDFLAGS) $(LDLIBS) -lm

$(THREAD_TESTS): %: %.c $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -pthread -L. -Wl,-rpath,$(CURDIR) \
	    -lcmetta $(LDFLAGS) $(LDLIBS) -lm

# Every MT_API declaration must have a definition in the library. A header and
# an implementation drift apart silently: an edit that removes a function
# leaves its declaration behind, and nothing notices until a consumer that
# happens to call it fails to link. This caught six functions deleted by an
# over-wide edit [measured 2026-08-28].
surface: $(LIB)
	@python3 -c "import re,subprocess,sys; \
	  d=set(re.findall(r'^MT_API[^;(]*?\b(mt_[a-z_0-9]+)\(', open('cmetta.h').read(), re.M)); \
	  o=subprocess.run(['nm','-D','--defined-only','$(LIB)'],capture_output=True,text=True).stdout; \
	  f={l.split()[2] for l in o.splitlines() if len(l.split())==3 and l.split()[1]=='T'}; \
	  miss=sorted(d-f); \
	  sys.exit('declared but not defined: '+', '.join(miss)) if miss else \
	  print(f'surface: {len(d)} declarations, all defined')"

# Every mt_/MT_ name the prose uses must EXIST in the header. The docs are the
# only consumer of this surface that no compiler reads, so a door that is
# renamed or retired leaves them describing an API nobody can call: the struct
# rewrite retired mt_each_cursor, mt_answer_text and mt_group and left all
# three in README.md and llms.txt, where they sat until a search found them
# [measured 2026-08-28; planting the two retired names back makes this fail
# naming exactly them]. This checks EXISTENCE and not call shape, which is the
# part a regex can answer honestly.
#
# The door, for a name the prose means to use without a header counterpart:
# write `<!-- names: <identifier> <why> -->` in the document itself. Declaring
# it in place keeps the reason beside the name rather than in this file.
docs:
	@python3 -c "import re,sys; \
	  known=set(re.findall(r'\b(?:mt_[a-z_0-9]+|MT_[A-Z_0-9]+)\b', open('cmetta.h').read())); \
	  bad=[]; \
	  [bad.extend((d,n) for n in sorted(set(re.findall(r'\b(?:mt_[a-z_0-9]+|MT_[A-Z_0-9]+)\b', open(d).read())) \
	    - known - set(re.findall(r'<!--\s*names:\s*(\S+)', open(d).read())))) \
	   for d in ('README.md','llms.txt')]; \
	  sys.exit('documented but not in cmetta.h: ' + ', '.join(f'{d}:{n}' for d,n in bad)) if bad else \
	  print('docs: every mt_ name in README.md and llms.txt is in the header')"

# The examples run too. An example that no longer compiles, or that compiles
# and then fails, is documentation that lies. The README quotes these programs;
# the Python seat gates its examples for the same reason.
test: $(TESTS) $(EXAMPLES) $(KIT) surface docs version hardening
	@./tests/test_cmetta
	@./tests/test_bad_boot
	@set -e; \
	fixture="$(TEST_TMP)/cmetta-path-o'brien-unicodé-$$$$"; \
	mkdir -p "$(TEST_TMP)"; \
	trap 'rm -f "$$fixture"' 0 1 2 15; \
	rm -f "$$fixture"; \
	ln -s "$(ENGINE_PATH)" "$$fixture"; \
	CMETTA_TEST_ENGINE_PATH="$$fixture" ./tests/test_quoted_path
	@./tests/test_qlf_boot
	@./tests/test_alloc_failure
	@./tests/test_cursor_ids
	@./tests/test_reopen
	@./tests/test_internal_contracts
	@./tests/test_batch_add
	@./tests/test_unify
	@./tests/test_seam
	@./tests/test_ownership
	@./tests/test_iterators
	@./tests/test_transactions
	@./tests/test_providers
	@./tests/test_native_parity
	@./tests/test_subscriptions
	@./tests/test_extensions
	@./tests/test_hash
	@./tests/test_threads
	@python3 ./tests/test_kit.py ./kit/driver "$(TEST_TMP)"
	@for example in $(EXAMPLES); do \
	    ./$$example > /dev/null || { echo "$$example failed" >&2; exit 1; }; \
	    echo "$$example ok"; \
	done

# Compare the header-derived version with the loaded library's answer.
version: $(LIB)
	@set -e; mkdir -p "$(TEST_TMP)"; \
	trap 'rm -f "$(TEST_TMP)/version-probe" "$(TEST_TMP)/version-probe.c"' 0 1 2 15; \
	printf '#include "cmetta.h"\n#include <stdio.h>\nint main(void){puts(mt_version());return 0;}\n' > "$(TEST_TMP)/version-probe.c"; \
	$(CC) $(CFLAGS) -o "$(TEST_TMP)/version-probe" "$(TEST_TMP)/version-probe.c" -L. -Wl,-rpath,$(CURDIR) -lcmetta $(LDFLAGS) $(LDLIBS) -lm; \
	reported=$$("$(TEST_TMP)/version-probe"); \
	if [ "$$reported" != "$(VERSION)" ]; then \
	    echo "mt_version() says $$reported and the Makefile says $(VERSION); \
they name the same release and must agree" >&2; exit 1; \
	fi; \
	echo "version: $(VERSION), and mt_version() agrees"

# These are link properties, so ask the linked object rather than trusting the
# flags above. Both are cheap and stay in the ordinary test dependency graph.
hardening: $(LIB) $(FAULT_LIB)
	@for library in $(LIB) $(FAULT_LIB); do \
	    readelf -lW $$library | grep -q 'GNU_RELRO' || { \
	        echo "$$library has no GNU_RELRO segment" >&2; exit 1; }; \
	    readelf -dW $$library | grep -Eq 'BIND_NOW|FLAGS.*NOW' || { \
	        echo "$$library does not request immediate binding" >&2; exit 1; }; \
	done
	@echo "hardening: GNU_RELRO and BIND_NOW on both shared libraries"

# AddressSanitizer cannot model SWI's private stacks. UBSan and standalone
# LeakSanitizer can, and sanitize.sh keeps their differently instrumented
# objects outside this directory so no target can silently reuse the wrong one.
sanitize:
	@sh ./sanitize.sh

# A runtime defect must remain reproducible independently of the binding.
# This gate retains every scenario's log and fails on any Memcheck error.
# [tested: make runtime-memory; commit=1a60e2a3cce69d5d6bda67100186939d707f4397]
$(TEST_TMP)/swi-memory-probe: tests/swi_memory_probe.c
	@mkdir -p "$(TEST_TMP)"
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

runtime-memory: $(TEST_TMP)/swi-memory-probe
	@status=0; for scenario in baseline int64 unicode; do \
	    result=0; \
	    DEBUGINFOD_URLS= TMPDIR="$(TEST_TMP)" valgrind \
	        --leak-check=full --show-leak-kinds=all \
	        --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
	        --log-file="$(TEST_TMP)/swi-$$scenario-valgrind.log" \
	        "$(TEST_TMP)/swi-memory-probe" "$$scenario" || result=$$?; \
	    printf 'SWI memory %s: exit %s\n' "$$scenario" "$$result"; \
	    if [ "$$result" -ne 0 ]; then status=$$result; fi; \
	done; exit "$$status"

# The installed library is a DIFFERENT build: it bakes the installed engine's
# path rather than this checkout's, so a program linked against it boots
# without $METTA_PATH set. That is the same bargain setup.py makes when it
# copies the runtime into the wheel.
# The baked engine path is a VARIABLE, not a file, so make cannot see it
# change: `make install PREFIX=/a` followed by `make install PREFIX=/b` shipped
# /a's path inside /b's library, because the .so was newer than its two sources
# both times. The stamp turns the configuration into a prerequisite, which is
# the ordinary way a Makefile notices one, and it rewrites only when the value
# actually differs so an unchanged prefix relinks nothing.
.enginedir-stamp: FORCE
	@printf '%s' '$(enginedir)' | cmp -s - $@ 2>/dev/null || printf '%s' '$(enginedir)' > $@
FORCE:

$(SOFILE): cmetta.c cmetta.h .enginedir-stamp
	$(CC) $(filter-out -DMT_ENGINE_PATH=%,$(CFLAGS)) \
	    -DMT_ENGINE_PATH='"$(enginedir)"' -shared -Wl,-soname,$(SONAME) \
	    -o $@ cmetta.c $(LDFLAGS) $(LDLIBS)

# Directory overrides are inputs even when no source file changes.
# [tested: make install-check; commit=1a60e2a3cce69d5d6bda67100186939d707f4397]
cmetta.pc: Makefile cmetta.h FORCE
	@printf '%s\n' \
	    'prefix=$(PREFIX)' \
	    'exec_prefix=$${prefix}' \
	    'libdir=$(libdir)' \
	    'includedir=$(includedir)' \
	    'enginedir=$(enginedir)' \
	    '' \
	    'Name: cmetta' \
	    'Description: MeTTa from C: an embedded MeTTa engine and its term API' \
	    'URL: https://github.com/MesTTo/MeTTa-Kernel' \
	    'Version: $(VERSION)' \
	    'Libs: -L$${libdir} -lcmetta' \
	    'Cflags: -I$${includedir} -std=c11' > $@

# The engine tree, its libraries, and this seat's own control file, which is
# what the engine globs to find the C bridge. The .qlf files are NOT installed:
# a shipped one shadows the source it was compiled from and ties the install to
# the builder's SWI version, which is the same reasoning MANIFEST.in gives for
# leaving them out of the sdist. The .so artifacts ARE installed, unlike the
# py3-none-any wheel's, because this install is for one platform by
# construction.
install: $(SOFILE) cmetta.pc version
	install -d $(DESTDIR)$(libdir) $(DESTDIR)$(includedir) \
	           $(DESTDIR)$(pkgconfigdir) $(DESTDIR)$(enginedir)
	install -m 755 $(SOFILE) $(DESTDIR)$(libdir)/$(SOFILE)
	ln -sf $(SOFILE) $(DESTDIR)$(libdir)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(libdir)/$(LIB)
	install -m 644 cmetta.h $(DESTDIR)$(includedir)/cmetta.h
	install -m 644 cmetta.pc $(DESTDIR)$(pkgconfigdir)/cmetta.pc
	cd "$(ENGINE_PATH)" && find engine lib -type f \
	    ! -name '*.qlf' ! -name '.qlf-stamp' ! -name '*.o' \
	    ! -path '*/__pycache__/*' \
	    -exec install -Dm 644 {} $(DESTDIR)$(enginedir)/{} \;
	cd "$(ENGINE_PATH)" && find engine lib -type f -name '*.so' \
	    -exec install -Dm 755 {} $(DESTDIR)$(enginedir)/{} \;
	install -Dm 644 extension.pl $(DESTDIR)$(enginedir)/extensions/cmetta/extension.pl
	install -Dm 644 bridge.pl $(DESTDIR)$(enginedir)/extensions/cmetta/bridge.pl
	@echo "installed cmetta $(VERSION) under $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(libdir)/$(SOFILE) $(DESTDIR)$(libdir)/$(SONAME) \
	      $(DESTDIR)$(libdir)/$(LIB) $(DESTDIR)$(includedir)/cmetta.h \
	      $(DESTDIR)$(pkgconfigdir)/cmetta.pc
	rm -rf $(DESTDIR)$(enginedir)

# An install nobody links against is an install nobody has tested. This stages
# one under build/, compiles a consumer that knows only what pkg-config says,
# and runs it with no METTA_PATH and no rpath into this checkout, which is the
# whole claim: a program outside this tree can boot the engine.
install-check:
	rm -rf build/install-check
	$(MAKE) cmetta.pc PREFIX=$(CURDIR)/ai-tmp/cmetta-parity/previous-prefix
	# PREFIX rather than DESTDIR, and that is the whole point: DESTDIR only
	# STAGES a tree whose paths still say /usr, so a staged copy cannot run in
	# place -- the library boots, looks where it was told, and refuses by name.
	# A real prefix under build/ is an install that is genuinely installed,
	# needs no root, and is the configuration a consumer meets.
	$(MAKE) install PREFIX=$(CURDIR)/build/install-check
	@test "$$(PKG_CONFIG_PATH=$(CURDIR)/build/install-check/lib/pkgconfig pkg-config --variable=prefix cmetta)" = "$(CURDIR)/build/install-check"
	@cd build/install-check && \
	    export PKG_CONFIG_PATH=$(CURDIR)/build/install-check/lib/pkgconfig && \
	    flags=$$(pkg-config --cflags --libs cmetta) && \
	    $(CC) -Wall -Wextra -Wpedantic -Werror -o consumer $(CURDIR)/tests/install_consumer.c $$flags \
	        -Wl,-rpath,$(CURDIR)/build/install-check/lib
	@readelf -dW build/install-check/consumer | grep -F 'Shared library: [$(SONAME)]'
	@answer=$$(env -u METTA_PATH ./build/install-check/consumer); \
	if [ "$$answer" != "5" ]; then \
	    echo "an installed consumer answered '$$answer', wanted 5" >&2; exit 1; \
	fi
	@echo "install-check: a consumer outside this checkout booted the installed engine"

clean:
	rm -f $(LIB) $(FAULT_LIB) $(SOFILE) cmetta.pc .enginedir-stamp \
	      tests/extension_accept.so tests/extension_refuse.so \
	      .version-probe .version-probe.c \
	      $(EXAMPLES) $(TESTS) $(KIT) $(BENCH)
	rm -rf build/install-check
