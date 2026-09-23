"""Purpose: hold what a C host pays for this binding to committed counters.

Every case here runs benchmarks/cases, the C driver beside this file, once per
sample under `perf stat` and once more under Cachegrind, and compares three
counters against benchmarks/baseline.json through metta's own
BenchmarkBaseline. The harness is
imported, never copied: DEVELOPING.md's rule is that a sibling package takes
BenchmarkBaseline, benchmark_case, count_atoms and measure_instructions from
metta.testing.

THE COUNTER RULE FOR THIS SEAT. Inference counters are BLIND across the C
boundary, because foreign code retires no inferences at all. This tree has the
failure on record: a C wire encoder measured 526x faster on the inference
counter while CPU time said it was 1.8x SLOWER. So every case that crosses into
C is decided by `perf stat -e instructions:u` and Cachegrind's estimated cycles
PAIRED, never by inferences: the second sees what the first cannot, a change
that keeps every instruction and wrecks the memory behaviour behind them.
Inferences are pinned as well, because each case's count measured exactly
reproducible, and they answer a different question: what the ENGINE did per
operation. A case comment says which counter decides it.

Time decides nothing here. task-clock is recorded per operation beside the
pins, as advice, because a time reading prices the queue as well as the work
and the box these gates run on is never quiet; wall clock is not recorded.

Owns resources: subprocess.run reaps warmup children. prepare_boot removes
the boot's governed QLF caches; ordinary boot and import recreate their artifacts.
[tested: test_c_boot_normalises_the_governed_cache_set; commit=8ca8a387fc61d0918484b19a1a3baf85b6523043]

Guarantees:
  - boot purges its governed QLF caches, warms the ordinary engine artifact set,
    and fails if its governed count differs from the recorded fixture
    [tested: test_c_boot_normalises_the_governed_cache_set,
    test_c_inventory_failure_is_fatal_and_runtime_still_compares; commit=8ca8a387fc61d0918484b19a1a3baf85b6523043]
  - every boot counter declines a different declared checkout length or
    depth, including updates; runtime rows still compare [tested:
    test_boot_path_refuses_both_counters_and_preserves_pins,
    test_comparable_counters_still_gate; commit=8ca8a387fc61d0918484b19a1a3baf85b6523043]
  - a box that would not count is told apart from a tree that moved: this
    lane exits 0 with a named skip on a developer's box and 1 where CI=true,
    and never reports a refused measurement as a moved row
    [tested: test_a_benchmark_lane_skips_a_refusal_locally_and_refuses_it_in_ci;
    commit=11afdcdbad5bbbe37168b5d8528c23a21c42b4b6]
  - one process per case, so a case never measures a runtime another case
    warmed [source: extensions/cmetta/benchmarks/cases.c, one runtime per
    process]
  - setup and boot sit outside the counted region for every case but `boot`,
    through perf's control descriptors and, under Cachegrind, the driver's
    client requests around the same operation, so a per-operation row prices
    the operation rather than the engine start in front of it
  - the simulated runs start only once every perf sample is taken, so none of
    them loads an artifact set boot's purge is rewriting; they then go at once,
    since no other process can move a simulated count
  - a regression in one case never hides another: every selected case is
    measured and every failure is reported before the nonzero exit, the shape
    benchmarks/check_instructions.py already established after
    stop-at-first-failure masked four stale pins
  - the engine is warmed once before any sample, because a stale engine/*.qlf
    is recompiled by the first boot that meets it and that run would carry a
    compile the others do not
  - every sample runs from the same fixed directory whoever invoked it and
    from where, because the caller's working directory is inherited and moves
    the boot count by more than its own band; see anchor()
Open Obligations:
  To Do: None
  Hacks: None
  Future Enhancements: None
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections.abc import Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from functools import partial
from pathlib import Path

SEAT = Path(__file__).resolve().parents[1]
ROOT = SEAT.parents[1]
# The seat is not installed; it resolves from the tree, the same way check.sh
# reaches it by running the Python seat's own runners from inside that seat.
# The harness is a distribution of its own under that seat's ext/, so the
# entry above only makes _workspace reachable and on_path() does the rest.
sys.path.insert(0, str(ROOT / "extensions" / "python"))

from _workspace import on_path  # noqa: E402  -- the path entry above

on_path()

from metta_benchmarking import (  # noqa: E402  -- on_path() above is what makes this import resolvable
    ESTIMATED_CYCLES,
    INSTRUCTIONS,
    BenchmarkBaseline,
    estimated_cycles,
    measure_counters,
    measure_simulated,
    measured_main,
    prepare_governed_artifacts,
    refusal_is_fatal,
)

DRIVER = SEAT / "benchmarks" / "cases"
BASELINE = SEAT / "benchmarks" / "baseline.json"
#: task-clock is CPU time, not wall time, and comes from the same perf run as
#: instructions:u so the advice beside a pin describes the run that set it.
EVENTS = ("instructions:u", "task-clock")
#: What this document says about itself, replacing the default seat's sentence
#: that inferences decide. They cannot decide here, and a committed file that
#: said they did would be wrong about every row under it.
POLICIES = {
    "counter_policy": (
        "instructions:u and estimated cycles, each the minimum of three, DECIDE "
        "every row, paired: foreign code retires no inferences, so the engine's "
        "counter is blind to this binding's own work and is pinned only as a "
        "third reading of what the ENGINE did. Time decides nothing: task-clock "
        "is recorded per operation as advice, and wall clock is not recorded."
    ),
    "instruction_policy": (
        "perf instructions:u minimum of three under setarch -R and a built "
        "environment, banded on both sides by each row's own declared percent; "
        "every per-operation row excludes the engine boot in front of it "
        "through perf's control descriptors, and `boot` is the whole process "
        "on purpose"
    ),
    "estimated_cycles_policy": (
        "Cachegrind's simulated cost of the same window, minimum of three, "
        "banded on both sides by each row's own declared percent: an access "
        "costs one on a first-level hit, five on a last-level hit and "
        "thirty-five from memory, over a fixed 32 KiB 8-way first-level pair "
        "and an 8 MiB 16-way last level. It exists to catch what an "
        "instruction count cannot see, a change that keeps every instruction "
        "and wrecks the memory behaviour behind them, and it is simulated "
        "because no other process on the box can move a simulated count."
    ),
}


@dataclass(frozen=True)
class Case:
    """One workload, its size, and what its numbers mean."""

    name: str
    unit: str
    operations: int
    #: Measured as the whole process rather than inside the control window,
    #: which only `boot` needs and only because it IS the process. The C
    #: driver refuses the pairing the other way round, so the two sides cannot
    #: disagree about which case this is.
    whole_process: bool = False


#: The sizes are what a simulated run can afford: each counted region retires
#: 170 to 650 million instructions, so a run under Cachegrind takes four to six
#: seconds, most of it the uninstrumented boot in front of the window, while
#: the instruction band still dwarfs perf's own window edges, about 16,000
#: instructions of handshake [measured 2026-09-23: windowed Cachegrind runs of
#: every case at these sizes; the MORK seat's mork-window-floor calibration row].
#: They are a tenth of the sizes task-clock needed when it decided, since a
#: region under a millisecond measured 86% spread in time on this box.
#: Every case measured LINEAR in its size at 2,000 and 20,000 operations, so
#: the size is a lever on cost and not on what the row means: term-in read
#: 71,188 instructions per operation at 2,000 and 71,173 at 20,000, space-pair
#: 101,308 and 102,256, and both inference counts came out exactly ten times
#: apart [measured 2026-08-28].
CASES = (
    # boot. What a C host pays before it can ask anything: the dynamic loader,
    # PL_initialise, and consulting the engine. DECIDED BY instructions:u AND
    # ESTIMATED CYCLES. The inference pin sees only the consult, about 585,000
    # inferences of a 1.6G-instruction process, so it can neither confirm nor
    # deny the rest; it is here because a change in what the engine loads is
    # worth catching. This is the one case measured as a WHOLE PROCESS: a
    # control window opened inside main() would start after the loader had
    # already run.
    Case("boot", "boots", 1, whole_process=True),
    # cursor-step. One mt_next, which is one metta_c_next plus the decode of
    # its answer into a C atom and the render of its text. DECIDED BY
    # instructions:u AND ESTIMATED CYCLES, and this case is the counter rule in
    # one number: the engine retires 12 inferences per answer while the process
    # retires about 18,000 instructions, so what the inference counter can see
    # is a rounding error on what the step costs. Its pin still earns its place
    # -- it catches a change in the engine's per-answer reduction -- but it
    # cannot referee the C half at all.
    Case("cursor-step", "steps", 20_000),
    # term-in. A term crossing FROM C INTO the engine: mt_show_dup encodes a C
    # atom into a Prolog term and asks the engine to write it, the only public
    # door that crosses this way without also storing or evaluating something.
    # DECIDED BY instructions:u AND ESTIMATED CYCLES; the encode is pure C and
    # retires nothing, so the inference pin here prices only the writer on the
    # far side.
    Case("term-in", "crossings", 6_000),
    # term-out. The mirror: mt_parse runs the engine's reader and decodes
    # the resulting Prolog term into a C atom. Same term, same text door,
    # opposite crossing, and the pair is what makes the two rows comparable.
    # DECIDED BY instructions:u AND ESTIMATED CYCLES, for the same reason.
    Case("term-out", "crossings", 6_000),
    # space-pair. Store one fact and retrieve it by its key, which is what a C
    # host does with a space. DECIDED BY instructions:u AND ESTIMATED CYCLES.
    # The inference pin is the most informative one in the suite, because both
    # doors are engine work: the add asserts and the match runs the engine's
    # own matcher, so a matcher change lands here first.
    Case("space-pair", "pairs", 2_000),
    # error-ball. An engine exception crossing back to C as words: the engine
    # raises, call_bridge copies the ball off the stacks with PL_record, and
    # render_ball asks metta_c_error_text/2 for its text. DECIDED BY
    # instructions:u AND ESTIMATED CYCLES. A failed assertion is the raiser
    # because MeTTa keeps most failures AS values, so nothing else reaches this
    # path [source: extensions/cmetta/tests/test_cmetta.c,
    # test_an_engine_error_reaches_c_as_words]. The engine also reports each
    # failure on stderr, and that report is inside the region on purpose: a C
    # host pays for it.
    Case("error-ball", "raises", 200),
)

BY_NAME = {case.name: case for case in CASES}


def loaded_seats() -> list[str]:
    """Which seats a boot with the `extensions` token actually loads.

    ASKED, not modelled. Whether a seat loads is the loader's decision over its
    control file's needs -- an artefact on disk, a Prolog library on the search
    path, a predicate some host registered first, another seat -- and a second
    implementation of that rule here would be a copy that drifts from the one
    that decides. One boot costs about a seventh of a second, once per run.
    """
    answer = subprocess.run(
        [
            "swipl", "-q",
            "-g", "findall(S, metta_extension_loaded(S), Ss), sort(Ss, Sorted), "
                  "forall(member(Seat, Sorted), format('~w~n', [Seat]))",
            "-t", "halt",
            str(ROOT / "engine" / "metta.pl"), "--", "extensions",
        ],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    if answer.returncode != 0:
        detail = (answer.stderr or answer.stdout).strip()
        msg = f"could not ask the engine which seats load: {detail}"
        raise SystemExit(msg)
    return sorted(line for line in answer.stdout.split() if line)


#: What a seat DECLARES it loads, and what those files load in turn. Both are
#: read from the seat rather than listed here, so a seat that grows a unit is
#: covered without this file changing.
SEAT_ENTRY = re.compile(r"^entry\([a-z_]+,\s*'([^']+)'\)", re.MULTILINE)
SEAT_LOAD = re.compile(r"(?:ensure_loaded|consult)\('([^']+\.pl)'\)")


def seat_prolog_files(seat: str) -> list[Path]:
    """Every Prolog file a seat boots, from its own declarations outwards.

    extension.pl names its entries and each entry may load more, so the walk is
    transitive with a visited set. A path that escapes the seat is dropped: the
    engine's own sources are not this seat's configuration, and they move every
    pin here anyway.
    """
    directory = ROOT / "extensions" / seat
    manifest = directory / "extension.pl"
    if not manifest.is_file():
        return []
    pending = [manifest]
    seen: dict[Path, None] = {}
    while pending:
        path = pending.pop()
        resolved = path.resolve()
        if resolved in seen or not path.is_file():
            continue
        seen[resolved] = None
        text = path.read_text(encoding="utf-8", errors="replace")
        named = SEAT_ENTRY.findall(text) if path == manifest else []
        named += SEAT_LOAD.findall(text)
        for relative in named:
            candidate = (path.parent / relative).resolve()
            if candidate.is_relative_to(directory.resolve()):
                pending.append(candidate)
    return sorted(seen)


def seats_differing_from_head() -> list[str]:
    """Loaded seats whose declared Prolog differs from its committed state.

    Read only when a row has ALREADY failed, and never as a refusal. A seat's
    Prolog joins the engine's shared multifile seams, so its content is on this
    seat's measured path and not just its presence: the Node bridge's own
    seam:foreign_space/1 cost one inference on every space operation, consulted
    by the matcher, the type resolvers, the translator and the codec, which
    CHANGELOG.md records against that seat's own benchmark. A seat edited since
    the pin was taken can therefore move counters here with no change in this
    seat at all, and the failure then reads as a regression in the wrong tree.

    Against HEAD rather than a stamp in the baseline, for two reasons. A stamp
    would REFUSE every comparison while a sibling seat is being worked on,
    which is a false failure this seat would be inventing; and a stamp needs
    re-pinning where this needs nothing. What it answers is the question a
    reader of a red row actually has: is anything loaded here uncommitted?
    """
    moved: list[str] = []
    for seat in loaded_seats():
        for path in seat_prolog_files(seat):
            relative = path.relative_to(ROOT).as_posix()
            committed = subprocess.run(
                ["git", "show", f"HEAD:{relative}"],
                cwd=ROOT, capture_output=True,
            )
            if committed.returncode != 0 or committed.stdout != path.read_bytes():
                moved.append(f"{seat} ({relative})")
                break
    return moved


def counter_configuration() -> dict[str, bool | list[str] | str]:
    """The artifacts that move THIS seat's counters, for the baseline stamp.

    Deterministic counters only compare within one configuration. The engine's
    optional C reader and C writer are on the measured path directly here --
    term-out runs the reader through metta_c_read and term-in runs the writer
    through metta_c_show -- and all three artifacts change what a boot loads,
    so `boot` moves with any of them.

    Two keys the Python seat's stamp carries are deliberately absent. Its
    `c_extension` gates a Python row and nothing here. And the METTA_C_READER,
    METTA_C_WRITER and METTA_C_JSON overrides are not read at all, because
    every measurement here runs in a CHILD and measure_counters builds that
    child's environment from an allowlist of PATH, HOME, LD_LIBRARY_PATH and
    SWI_HOME_DIR: an override set in this process cannot reach the run it would
    describe, so stamping it would refuse a comparison over a difference that
    changed no number.

    The SEATS are the fourth key, and the one this stamp was missing. A C host
    boots with the `extensions` token, so every seat whose declared needs hold
    loads into the process being measured: the MORK backend alone costs 23,155
    inferences at boot and two per space operation, because its provider joins
    the ownership seam every add and match consults. These pins were first taken
    in a worktree that had no MORK artifacts, and against a tree that has them
    the difference reads as a 1.56% boot regression and a 3.77% space-pair one
    that no code caused [measured 2026-08-28: boot 1,493,506 inferences with
    seats [node, python] against 1,516,661 with [mork, node, python], same tree,
    same command]. Reading the seats rather than the artifacts keeps the key
    true for a seat that is present and unbuildable, and for one added later.

    The SIMULATOR is the fifth. Estimated cycles are Cachegrind's counts, and
    another valgrind decodes instructions and models caches in its own way, so
    it moves every one of those pins with no change here; its version is
    stamped for the reason the seats are, and a box without it stamps `absent`,
    which refuses by name rather than failing inside the first simulated run.
    """
    return {
        "c_reader": (ROOT / "engine" / "reader.so").is_file(),
        "c_writer": (ROOT / "engine" / "writer.so").is_file(),
        "c_json": (ROOT / "engine" / "json_codec.so").is_file(),
        "seats": loaded_seats(),
        "valgrind": simulator_version(),
    }


def simulator_version() -> str:
    """The valgrind that simulates the estimated-cycle rows, as it names itself."""
    try:
        answer = subprocess.run(
            ["valgrind", "--version"], capture_output=True, text=True, timeout=60, check=False
        )
    except FileNotFoundError:
        return "absent"
    return answer.stdout.strip() or "absent"


def command_for(case: Case) -> list[str]:
    """The driver invocation for one case."""
    words = [str(DRIVER), case.name, str(case.operations)]
    if not case.whole_process:
        words.append("--controlled")
    return words


def inferences_from(output: str) -> int:
    """The engine counter the driver printed for its own counted region."""
    for line in output.splitlines():
        if line.startswith("inferences "):
            return int(line.split()[1])
    msg = f"the driver printed no inference count: {output!r}"
    raise RuntimeError(msg)


def simulate(case: Case, rounds: int) -> tuple[int, ...]:
    """Estimated cycles, one per simulated run of the case."""
    runs = measure_simulated(
        command_for(case),
        rounds=rounds,
        controlled=not case.whole_process,
        timeout=600.0,
    )
    return tuple(
        estimated_cycles({event: counts[index] for event, counts in runs.events.items()})
        for index in range(rounds)
    )


def sample(case: Case, rounds: int) -> tuple[tuple[int, ...], tuple[float, ...], tuple[int, ...]]:
    """Instructions, CPU seconds and inferences, one of each per run."""
    runs = measure_counters(
        command_for(case),
        events=EVENTS,
        rounds=rounds,
        controlled=not case.whole_process,
        timeout=300.0,
    )
    return (
        tuple(int(value) for value in runs.events["instructions:u"]),
        #perf reports task-clock in milliseconds to two decimal places, so five
        #decimal places of seconds is the same number without binary-float
        #fringe, and the pin is in seconds because that is what a reader
        #compares against a stopwatch.
        tuple(round(value / 1000.0, 5) for value in runs.events["task-clock"]),
        tuple(inferences_from(text) for text in runs.outputs),
    )


def observe_all(
    baseline: BenchmarkBaseline, cases: Sequence[Case], rounds: int
) -> tuple[list[str], list[str]]:
    """Observe every case on every counter, returning failures and refusals.

    Each counter is compared SEPARATELY rather than in one try block. Stopping
    at the first would let an instruction regression hide an estimated-cycle
    regression on the same row, and the two only decide together: they are
    here precisely because each sees what the other cannot. It is the masking
    benchmarks/check_instructions.py was fixed for one level up, where it was
    one case hiding another.

    The perf samples come first, case by case, because boot's purge rewrites
    the artifact set every case loads. The simulated runs of every case that
    got that far then go at once: nothing another process does can move a
    simulated count, and each run costs twenty to fifty times its native one.
    task-clock is recorded per operation beside the pins and never compared.
    """
    failures: list[str] = []
    refused: list[str] = []
    # Every boot counter depends on the declared checkout shape. Equal-length
    # depth controls and a fresh-atom control name inventory sensitivity, not
    # a linear inference cost per component. The baseline owns that evidence.
    path_refusal = baseline.checkout_path_refusal(ROOT)
    sampled: dict[str, tuple[tuple[int, ...], tuple[float, ...], tuple[int, ...]]] = {}
    for case in cases:
        if case.whole_process:
            try:
                prepare_boot(baseline.cases[case.name].get("boot_qlf_count"))
            except (AssertionError, KeyError, OSError, subprocess.CalledProcessError) as error:
                failures.append(f"{case.name}: REFUSED {error}")
                if isinstance(error, subprocess.CalledProcessError) and error.stderr:
                    failures[-1] += f"\n{error.stderr.strip()}"
                print(failures[-1])
                continue
        sampled[case.name] = sample(case, rounds)
    measured = [case for case in cases if case.name in sampled]
    with ThreadPoolExecutor(max_workers=max(1, len(measured))) as pool:
        simulated = dict(zip(
            (case.name for case in measured),
            pool.map(lambda case: simulate(case, rounds), measured),
            strict=True,
        ))
    for case in measured:
        instructions, cpu, inferences = sampled[case.name]
        cycles = simulated[case.name]
        outside: list[str] = []
        declined = case.whole_process and path_refusal is not None
        for counter, observe in (
            (
                "inferences",
                partial(
                    baseline.observe_counter,
                    case.name,
                    unit=case.unit,
                    operations=case.operations,
                    samples=inferences,
                ),
            ),
            (
                "instructions",
                partial(baseline.observe_measurement, case.name, INSTRUCTIONS, instructions),
            ),
            (
                "estimated cycles",
                partial(baseline.observe_measurement, case.name, ESTIMATED_CYCLES, cycles),
            ),
        ):
            if declined:
                refused.append(f"{case.name}: {counter} not compared; {path_refusal}")
                continue
            try:
                observe()
            except (AssertionError, KeyError) as error:
                outside.append(f"{case.name}: {error}")
        if not declined:
            baseline.observe_cpu(case.name, min(cpu) / case.operations)
        report = (
            f"{case.name}: instructions={list(instructions)} "
            f"estimated_cycles={list(cycles)} cpu={list(cpu)} "
            f"inferences={list(inferences)}"
        )
        #Both band directions land on the same tag, and so does a missing row,
        #so it names the outcome rather than one side of it. A row declined for
        #its checkout shape is NOT that: it says so in its own words, because a
        #reader scanning for what moved has to be able to tell a row that read
        #wrong from a row that was not read.
        if outside:
            print(f"{report} OUTSIDE BAND")
        elif declined:
            print(f"{report} NOT MEASURED IN THIS CONFIGURATION")
        else:
            print(report)
        failures += outside
    return failures, refused


def prepare_boot(expected: object) -> None:
    """Normalise the artifact set whose freshness walk is inside C boot.

    Library imports and the optional source observer leave ignored caches.
    The purge and the ordinary warm boot are the shared harness's
    prepare_governed_artifacts, the same preparation the engine's own boot
    row takes, so the two boot rows read one artifact state; the benchmark
    keeps no second glob or list. See the 2026-09-10 fixture control in the
    runtime-units journal.
    """
    if type(expected) is not int or expected < 1:
        msg = "boot has no valid boot_qlf_count fixture; record its governed inventory"
        raise AssertionError(msg)
    inventory = prepare_governed_artifacts(ROOT)
    if len(inventory) != expected:
        msg = (f"governed QLF inventory {len(inventory)}; pinned {expected}; "
               "attribute the engine artifact-set change before re-pinning")
        raise AssertionError(msg)
    print(f"boot fixture: {expected} governed QLF artifacts after the boot's purge and ordinary warmup")


def warm() -> None:
    """Boot once, unmeasured, so no sample pays for a stale engine/*.qlf.

    SWI recompiles a .qlf whose source is newer the first time a boot meets it,
    and that run carries a compile the others do not. One discarded boot is the
    whole fix, and it costs about a seventh of a second.
    """
    # A built binary from this tree, no shell, fixed arguments.
    subprocess.run(
        [str(DRIVER), "boot", "1"], check=True, stdout=subprocess.DEVNULL
    )


def anchor() -> None:
    """Measure from a FIXED directory, whoever called and from where.

    posix_spawn gives the child the parent's working directory, and the engine's
    boot instruction count moves with it: measured 2026-08-28, boot read
    1,973,077,636 from /tmp, 1,974,705,233 from the repository root and
    1,975,121,930 from this seat's own folder, a 0.104% span that is wider than
    boot's own 0.1% band. So a developer running this from one directory and the
    gate running it from another would disagree by more than a regression has
    to be to fail. The seat root is the anchor because it is the directory this
    component already builds and tests from, and it is the same reasoning
    measure_counters gives for BUILDING the child's environment rather than
    inheriting it.
    """
    os.chdir(SEAT)


def main(argv: Sequence[str] | None = None) -> int:
    """Measure the selected cases and update or compare their counters."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    names = tuple(case.name for case in CASES)
    parser.add_argument("cases", nargs="*", choices=names, default=names)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--update", action="store_true")
    arguments = parser.parse_args(argv)

    if not DRIVER.is_file():
        print(
            f"the C benchmark driver is not built at {DRIVER}; "
            f"run sh {SEAT / 'bench.sh'}",
            file=sys.stderr,
        )
        return 2

    baseline = BenchmarkBaseline(BASELINE, update=arguments.update, policies=POLICIES)
    try:
        baseline.observe_configuration(counter_configuration())
    except AssertionError as error:
        #A refusal, not a crash. The message already carries its remedy, and a
        #stack trace in a gate lane reads like the tree broke rather than like
        #the tree declined to compare two configurations.
        print(error, file=sys.stderr)
        return 1
    anchor()
    warm()
    failures, refused = observe_all(
        baseline, [BY_NAME[name] for name in arguments.cases], arguments.rounds
    )
    baseline.finish()
    #Printed either way, so a declined comparison is never silent; what
    #refusal_is_fatal decides is whether it is also red. On a runner it is: a
    #row nobody measured is a tripwire nobody read.
    for message in refused:
        print(f"NOT MEASURED IN THIS CONFIGURATION {message}", file=sys.stderr)
    if refused:
        print(
            f"{len(refused)} comparison(s) declined for the reasons above; "
            "every runtime row's comparisons ran",
            file=sys.stderr,
        )
        if refusal_is_fatal():
            failures = failures + refused
    if failures:
        for message in failures:
            print(message, file=sys.stderr)
        #Named beside the failure, because a reader of a red row here needs to
        #know whether anything ELSE loaded into the measured process is
        #uncommitted before reading the row as this seat's regression.
        moved = seats_differing_from_head()
        if moved:
            print(
                f"note: these seats load into the measured process and differ "
                f"from HEAD, so they may be what moved a row rather than this "
                f"seat: {', '.join(moved)}",
                file=sys.stderr,
            )
        print(f"{len(failures)} case(s) outside the band", file=sys.stderr)
        return 1
    if refused:
        print(f"{len(arguments.cases)} case(s) sampled; {len(refused)} comparison(s) declined")
    else:
        print(f"{len(arguments.cases)} case(s) within band")
    return 0


if __name__ == "__main__":
    raise SystemExit(measured_main(main))
