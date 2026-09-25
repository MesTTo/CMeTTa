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
#   Nothing under $PREFIX is version-control metadata, and install-check
#   refuses an install that carries any [tested: extensions/cmetta/check.sh
#   c-install; commit=f76b44e798301c09fd924cb4fec32f4ae0854e39].
# Decides: the engine tree is baked in as MT_ENGINE_PATH so a linked program
#   boots with no environment set, and $METTA_PATH still overrides it at run
#   time. A checkout that moves relinks at its next `make`, because the path is
#   part of the toolchain stamp, which is the same bargain setup.py makes when
#   it copies the runtime into the Python wheel.
#   Every ordinary build treats warnings as errors, refuses undefined shared-
#   library symbols, and emits stack-protected full-RELRO objects. `make
#   sanitize` rebuilds in root ai-tmp so sanitizer and ordinary objects never
#   contaminate one another [tested: GATE_ONLY=1 sh check.sh c-sanitize;
#   commit=b339084bb5625996fc88a31608d48ad31c575d1f].
# Guarantees: installed archive consumers receive the private SWI linker flags
#   [tested: make install-check; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5].
#   `make docs` refuses a soname README.md or llms.txt names other than SONAME,
#   and `make install-check` an install whose header or cmetta.pc names a
#   version other than VERSION, so a release edits only cmetta.h, those two
#   documents and CHANGELOG.md [tested 2026-09-25T19:27:30+10:00: make docs, make install-check].
# Open Obligations: None.

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

# A VARIABLE an output bakes in is an input make cannot see change: a target
# newer than its sources is up to date whatever it was built with. So each such
# value is written to a stamp that is rewritten only when the value differs,
# and every target baking it depends on the stamp, which is how git's own
# Makefile tracks its compiler flags (TRACK_CFLAGS and GIT-CFLAGS there,
# including this escape for the single quotes CFLAGS carries around
# MT_ENGINE_PATH). An unchanged value relinks nothing.
#
# `toolchain` is the whole command line the ordinary build compiles and links
# with. The part of it that moves is the SWI host: `swipl` on PATH decides
# PLBASE and PLLIBDIR, libcmetta.so links that libswipl by rpath, and the engine
# refuses an unpatched host at boot, so a library linked before the host
# changed kept the stock one and every program linking it died in
# PL_initialise [measured 2026-09-23: benchmarks/cases boot 1 raised
# error(metta_host_unpatched(..., none(/usr/lib/swi-prolog)), _) from a
# libcmetta.so linked at 16:21, before the refusal landed, and booted once
# relinked under the patched swipl; commit=681fdd8b07ed652d7e3798704c0bebce30094c47].
# ENGINE_PATH is the other part: a copy of this checkout, a battery tree
# included, used to keep the library linked in the original and so booted the
# ORIGINAL's engine rather than its own.
toolchain = $(CC) $(CFLAGS) $(LDFLAGS) $(LDLIBS)
.%-stamp: FORCE
	@printf '%s' '$(subst ','\'',$($*))' | cmp -s - $@ 2>/dev/null || \
	    printf '%s' '$(subst ','\'',$($*))' > $@
FORCE:

LIB       := libcmetta.so
# cmetta.h includes vocabularies.h, which extensions/python/tools/vocabgen.py
# generates from the engine's (vocabulary ...) rows, and settings.h, which
# extensions/python/tools/boundsgen.py generates from the Setting declarations
# that name an SWI flag; all three are the public header.
HEADERS   := cmetta.h vocabularies.h settings.h
STATIC_LIB := libcmetta.a
FAULT_LIB := tests/libcmetta_fault.so
EXAMPLES  := examples/hello examples/ops examples/stream examples/lower examples/language
FAULT_TESTS := tests/test_alloc_failure tests/test_cursor_ids tests/test_reopen \
               tests/test_internal_contracts tests/test_hash tests/test_stack_ceiling
THREAD_TESTS := tests/test_threads
TESTS     := tests/test_cmetta tests/test_bad_boot tests/test_quoted_path \
             tests/test_qlf_boot tests/test_batch_add tests/test_unify \
             tests/test_seam tests/test_ownership tests/test_iterators tests/test_transactions tests/test_providers tests/test_native_parity \
             tests/test_subscriptions tests/test_extensions tests/test_matchers \
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

# The soname carries the MAJOR version alone, libcmetta.so.<major>, so a
# consumer linked against it keeps working across compatible releases and
# stops linking when the surface breaks. Two files and two symlinks is the
# layout every ELF toolchain expects. VERSION is derived from the public header,
# `make version` checks the loaded library against it, `make install-check`
# checks the installed header and cmetta.pc, and `make docs` holds every
# soname README.md and llms.txt name to SONAME.
VERSION   := $(shell sed -n 's/^\#define MT_VERSION "\([^"]*\)"/\1/p' cmetta.h)
SOVERSION := $(firstword $(subst ., ,$(VERSION)))
SOFILE    := libcmetta.so.$(VERSION)
SONAME    := libcmetta.so.$(SOVERSION)

.PHONY: all test bench examples kit surface docs version hardening sanitize runtime-memory runtime-engineless-erase \
        runtime-halt-created-thread \
        install uninstall install-check clean FORCE

kit: $(KIT)

# The benchmark driver is a target of its own so bench.sh can ask for it
# without rebuilding the suite, and so `make all` still produces everything a
# fresh checkout needs.
bench: $(BENCH)

all: $(LIB) $(STATIC_LIB) examples $(KIT) $(BENCH)

# Archive consumers need the same implementation and transitive SWI dependency.
# [tested: make install-check; commit=91eef0753a3d55913cee42a2d385bbbf008f0be5]
cmetta.o: cmetta.c $(HEADERS) .toolchain-stamp
	$(CC) $(CFLAGS) -c -o $@ $<

$(STATIC_LIB): cmetta.o
	$(AR) rcs $@ $<

# Every other program here links one of these two libraries, so it follows
# them when the toolchain stamp moves.
$(LIB): cmetta.c $(HEADERS) .toolchain-stamp
	$(CC) $(CFLAGS) -shared -o $@ cmetta.c $(LDFLAGS) $(LDLIBS)

$(FAULT_LIB): cmetta.c $(HEADERS) .toolchain-stamp
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

$(THREAD_TESTS): %: %.c $(FAULT_LIB)
	$(CC) $(CFLAGS) -DMT_TEST_FAULTS -o $@ $< -pthread -Ltests \
	    -Wl,-rpath,$(CURDIR)/tests -lcmetta_fault $(LDFLAGS) $(LDLIBS) -lm

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

# Every mt_/MT_ name the prose uses must EXIST in a public header. The docs are the
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
# tests/readme_links.py then refuses a relative link in README.md, which the
# site publishes from another directory, where no such link resolves.
docs:
	@python3 -c "import re,sys; \
	  known=set(re.findall(r'\b(?:mt_[a-z_0-9]+|MT_[A-Z_0-9]+)\b', ''.join(open(h).read() for h in '$(HEADERS)'.split()))); \
	  bad=[]; \
	  [bad.extend((d,n) for n in sorted(set(re.findall(r'\b(?:mt_[a-z_0-9]+|MT_[A-Z_0-9]+)\b', open(d).read())) \
	    - known - set(re.findall(r'<!--\s*names:\s*(\S+)', open(d).read())))) \
	   for d in ('README.md','llms.txt')]; \
	  sys.exit('documented but in neither header: ' + ', '.join(f'{d}:{n}' for d,n in bad)) if bad else \
	  print('docs: every mt_ name in README.md and llms.txt is in a header')"
	@stale=$$(grep -o 'libcmetta\.so\.[0-9][0-9]*' README.md llms.txt | \
	    grep -v -x -F -e 'README.md:$(SONAME)' -e 'llms.txt:$(SONAME)'); \
	if [ -n "$$stale" ]; then \
	    echo "docs: README.md or llms.txt names a soname other than $(SONAME), which this Makefile builds:" >&2; \
	    echo "$$stale" >&2; exit 1; \
	fi; \
	echo "docs: every soname README.md and llms.txt name is $(SONAME)"
	@python3 tests/readme_links.py

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
	@./tests/test_stack_ceiling
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
	@./tests/test_matchers
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
# [tested: make runtime-memory; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
$(TEST_TMP)/swi-memory-probe: tests/swi_memory_probe.c .toolchain-stamp
	@mkdir -p "$(TEST_TMP)"
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

# The same rule for a host defect with no leak to count: SWI 10.1.14 erasing
# records on a thread with no Prolog engine faults once their atoms cross the
# atom-GC margin. cmetta's handle_release erases on such a thread, relying on
# the swi-gc-signal-engineless-thread patch the engine requires of its host
# since superproject 79a48d315, and this target is that patch's reproduction
# [measured 2026-09-24: died of SIGSEGV 3 runs of 3 on 10.1.14 without the
# patch; erased 20,000 records and exited 0 3 runs of 3 on swipl-patched.2].
$(TEST_TMP)/swi-engineless-erase-probe: tests/swi_engineless_erase_probe.c .toolchain-stamp
	@mkdir -p "$(TEST_TMP)"
	$(CC) $(CFLAGS) -pthread -o $@ $< $(LDFLAGS) $(LDLIBS)

runtime-engineless-erase: $(TEST_TMP)/swi-engineless-erase-probe
	@"$(TEST_TMP)/swi-engineless-erase-probe"

# A third host defect: SWI's halt passes over a thread still being created,
# which then runs while PL_cleanup frees the modules it looks its goal up in.
# One run dies only often, so the target runs twenty: at the measured rate an
# unpatched host passes all twenty with probability 0.2^20, about 1e-14
# [measured 2026-09-24: 16 runs of 20 died on swipl-patched.2].
$(TEST_TMP)/swi-halt-created-thread-probe: tests/swi_halt_created_thread_probe.c .toolchain-stamp
	@mkdir -p "$(TEST_TMP)"
	$(CC) $(CFLAGS) -pthread -o $@ $< $(LDFLAGS) $(LDLIBS)

runtime-halt-created-thread: $(TEST_TMP)/swi-halt-created-thread-probe
	@run=1; while [ $$run -le 20 ]; do \
	    "$(TEST_TMP)/swi-halt-created-thread-probe" > /dev/null || \
	        { echo "halt died in run $$run of 20"; exit 1; }; \
	    run=$$((run + 1)); \
	done; echo "halted past threads just created, 20 runs of 20"

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
# both times. .enginedir-stamp is the stamp rule near the top of this file
# applied to `enginedir`; the installed library also compiles and links with
# the ordinary toolchain, so it follows .toolchain-stamp as well.
$(SOFILE): cmetta.c $(HEADERS) .enginedir-stamp .toolchain-stamp
	$(CC) $(filter-out -DMT_ENGINE_PATH=%,$(CFLAGS)) \
	    -DMT_ENGINE_PATH='"$(enginedir)"' -shared -Wl,-soname,$(SONAME) \
	    -o $@ cmetta.c $(LDFLAGS) $(LDLIBS)

build/install/cmetta.o: cmetta.c $(HEADERS) .enginedir-stamp .toolchain-stamp
	@mkdir -p $(@D)
	$(CC) $(filter-out -DMT_ENGINE_PATH=%,$(CFLAGS)) \
	    -DMT_ENGINE_PATH='"$(enginedir)"' -c -o $@ $<

build/install/$(STATIC_LIB): build/install/cmetta.o
	$(AR) rcs $@ $<

# Directory overrides are inputs even when no source file changes.
# [tested: make install-check; commit=d353402e1d5db2345d5864fb3dfbf64bd39b180c]
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
	    'Libs.private: -L$(PLLIBDIR) -Wl,-rpath,$(PLLIBDIR) -lswipl -ldl -pthread' \
	    'Cflags: -I$${includedir} -std=c11' > $@

# The engine tree, its libraries, and this seat's own control file, which is
# what the engine globs to find the C bridge. The .qlf files are NOT installed:
# a shipped one shadows the source it was compiled from and ties the install to
# the builder's SWI version, which is the same reasoning MANIFEST.in gives for
# leaving them out of the sdist. The .so artifacts ARE installed, unlike the
# py3-none-any wheel's, because this install is for one platform by
# construction. Version-control metadata never ships: every `.git*` name is
# pruned, which is a submodule's `.git` (its whole repository in a main
# checkout, a one-line gitlink in a worktree), the `.gitignore` files and any
# `.github`, as the Python seat's sdist prunes `.git` directories [source:
# setuptools 84.0.0, setuptools/_distutils/command/sdist.py,
# prune_file_list]. Unpruned, an install from a main checkout carried 1,105
# files of lib's repository, and one from a worktree made the installed lib a
# nested repository `git clean -fdx` leaves behind [measured: 2026-09-24,
# find over wt-merge and battery 1].
install: $(SOFILE) build/install/$(STATIC_LIB) cmetta.pc version
	install -d $(DESTDIR)$(libdir) $(DESTDIR)$(includedir) \
	           $(DESTDIR)$(pkgconfigdir) $(DESTDIR)$(enginedir)
	install -m 755 $(SOFILE) $(DESTDIR)$(libdir)/$(SOFILE)
	install -m 644 build/install/$(STATIC_LIB) $(DESTDIR)$(libdir)/$(STATIC_LIB)
	ln -sf $(SOFILE) $(DESTDIR)$(libdir)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(libdir)/$(LIB)
	install -m 644 $(HEADERS) $(DESTDIR)$(includedir)/
	install -m 644 cmetta.pc $(DESTDIR)$(pkgconfigdir)/cmetta.pc
	cd "$(ENGINE_PATH)" && find engine lib -name '.git*' -prune -o -type f \
	    ! -name '*.qlf' ! -name '.qlf-stamp' ! -name '*.o' \
	    ! -path '*/__pycache__/*' \
	    -exec install -Dm 644 {} $(DESTDIR)$(enginedir)/{} \;
	cd "$(ENGINE_PATH)" && find engine lib -name '.git*' -prune -o -type f -name '*.so' \
	    -exec install -Dm 755 {} $(DESTDIR)$(enginedir)/{} \;
	install -Dm 644 extension.pl $(DESTDIR)$(enginedir)/extensions/cmetta/extension.pl
	install -Dm 644 bridge.pl $(DESTDIR)$(enginedir)/extensions/cmetta/bridge.pl
	@echo "installed cmetta $(VERSION) under $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(libdir)/$(SOFILE) $(DESTDIR)$(libdir)/$(SONAME) \
	      $(DESTDIR)$(libdir)/$(STATIC_LIB) \
	      $(DESTDIR)$(libdir)/$(LIB) $(addprefix $(DESTDIR)$(includedir)/,$(HEADERS)) \
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
	@leaked=$$(find build/install-check -name '.git*'); \
	if [ -n "$$leaked" ]; then \
	    echo "install-check: the install carries version-control metadata:" >&2; \
	    echo "$$leaked" >&2; exit 1; \
	fi
	@echo "install-check: the install carries no version-control metadata"
	@test "$$(PKG_CONFIG_PATH=$(CURDIR)/build/install-check/lib/pkgconfig pkg-config --variable=prefix cmetta)" = "$(CURDIR)/build/install-check"
	@installed=$$(PKG_CONFIG_PATH=$(CURDIR)/build/install-check/lib/pkgconfig pkg-config --modversion cmetta); \
	if [ "$$installed" != "$(VERSION)" ] || \
	   ! grep -qx '#define MT_VERSION "$(VERSION)"' build/install-check/include/cmetta.h; then \
	    echo "install-check: this checkout builds $(VERSION), and the install names $$installed" >&2; exit 1; \
	fi
	@echo "install-check: the installed header and cmetta.pc name $(VERSION)"
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
	@export PKG_CONFIG_PATH=$(CURDIR)/build/install-check/lib/pkgconfig; \
	    $(CC) -Wall -Wextra -Wpedantic -Werror \
	    $$(pkg-config --cflags cmetta) tests/install_consumer.c \
	    -o build/install-check/static-consumer \
	    -L$(CURDIR)/build/install-check/lib -Wl,-Bstatic -lcmetta -Wl,-Bdynamic \
	    $$(pkg-config --static --libs-only-L --libs-only-other cmetta) -lswipl -ldl -pthread
	@if readelf -dW build/install-check/static-consumer | grep -q 'Shared library: \[libcmetta'; then \
	    echo 'archive consumer unexpectedly requires libcmetta.so' >&2; exit 1; fi
	@test "$$(env -u METTA_PATH ./build/install-check/static-consumer)" = 5
	@echo "install-check: archive consumer booted the installed engine"

clean:
	rm -f $(LIB) $(STATIC_LIB) cmetta.o $(FAULT_LIB) $(SOFILE) cmetta.pc \
	      .enginedir-stamp .toolchain-stamp \
	      tests/extension_accept.so tests/extension_refuse.so \
	      .version-probe .version-probe.c \
	      $(EXAMPLES) $(TESTS) $(KIT) $(BENCH)
	rm -rf build/install-check build/install
