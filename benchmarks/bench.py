"""Purpose: hold what a C host pays for this binding to committed counters.

Every case here runs benchmarks/cases, the C driver beside this file, once per
sample under `perf stat`, and compares three counters against
benchmarks/baseline.json through metta's own BenchmarkBaseline. The harness is
imported, never copied: DEVELOPING.md's rule is that a sibling package takes
BenchmarkBaseline, benchmark_case, count_atoms and measure_instructions from
metta.testing.

THE COUNTER RULE FOR THIS SEAT. Inference counters are BLIND across the C
boundary, because foreign code retires no inferences at all. This tree has the
failure on record: a C wire encoder measured 526x faster on the inference
counter while CPU time said it was 1.8x SLOWER. So every case that crosses into
C is decided by `perf stat -e instructions:u` and CPU time PAIRED, never by
inferences. Inferences are pinned as well, because each case's count measured
exactly reproducible, and they answer a different question: what the ENGINE did
per operation. A case comment says which counter decides it.

Wall clock decides nothing here and is not recorded.

Guarantees:
  - one process per case, so a case never measures a runtime another case
    warmed [source: extensions/cetta/benchmarks/cases.c, one runtime per
    process]
  - setup and boot sit outside the counted region for every case but `boot`,
    through perf's control descriptors, so a per-operation row prices the
    operation rather than the engine start in front of it
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
from dataclasses import dataclass
from functools import partial
from pathlib import Path

SEAT = Path(__file__).resolve().parents[1]
ROOT = SEAT.parents[1]
# metta is not installed; it resolves from the tree, the same way check.sh
# reaches it by running the Python seat's own runners from inside that seat.
sys.path.insert(0, str(ROOT / "extensions" / "python"))

from metta.testing import (  # noqa: E402  -- the path above is what makes this import resolvable
    CPU_SECONDS,
    INSTRUCTIONS,
    BenchmarkBaseline,
    measure_counters,
)

DRIVER = SEAT / "benchmarks" / "cases"
BASELINE = SEAT / "benchmarks" / "baseline.json"
#: task-clock is CPU time, not wall time, and comes from the same perf run as
#: instructions:u so the pair describes one execution rather than two.
EVENTS = ("instructions:u", "task-clock")
#: What this document says about itself, replacing the default seat's sentence
#: that inferences decide. They cannot decide here, and a committed file that
#: said they did would be wrong about every row under it.
POLICIES = {
    "counter_policy": (
        "instructions:u and CPU time, each the minimum of three, DECIDE every "
        "row, paired: foreign code retires no inferences, so the engine's "
        "counter is blind to this binding's own work and is pinned only as a "
        "third reading of what the ENGINE did. Wall clock decides nothing and "
        "is not recorded."
    ),
    "instruction_policy": (
        "perf instructions:u minimum of three under setarch -R and a built "
        "environment, banded on both sides by each row's own declared percent; "
        "every per-operation row excludes the engine boot in front of it "
        "through perf's control descriptors, and `boot` is the whole process "
        "on purpose"
    ),
    "cpu_policy": (
        "perf task-clock minimum of three, seconds, banded on both sides by "
        "each row's own declared percent. It exists to catch what an "
        "instruction count cannot see, a change that keeps every instruction "
        "and wrecks the time they take, so its band is an order of magnitude "
        "looser than the instruction band and is not a precision figure."
    ),
}


@dataclass(frozen=True)
class Case:
    """One workload, its size, and what its numbers mean."""

    name: str
    unit: str
    operations: int
    #: Measured as the whole process rather than inside perf's control window,
    #: which only `boot` needs and only because it IS the process. The C
    #: driver refuses the pairing the other way round, so the two sides cannot
    #: disagree about which case this is.
    whole_process: bool = False


#: The sizes are chosen so each counted region is roughly 200ms of CPU, which
#: is where task-clock stops being dominated by its own resolution: a
#: sub-millisecond region measured 86% spread on this box under load where a
#: 58ms one measured 5.4% [measured 2026-08-28]. Every case measured LINEAR in
#: its size at 2,000 and 20,000 operations, so the size is a lever on precision
#: and not on what the row means: term-in read 71,188 instructions per
#: operation at 2,000 and 71,173 at 20,000, space-pair 101,308 and 102,256, and
#: both inference counts came out exactly ten times apart [measured 2026-08-28].
CASES = (
    # boot. What a C host pays before it can ask anything: the dynamic loader,
    # PL_initialise, and consulting the engine. DECIDED BY instructions:u AND
    # CPU TIME. The inference pin sees only the consult, which is 1.5M of a
    # 1.97G-instruction process, so it can neither confirm nor deny the rest;
    # it is here because a change in what the engine loads is worth catching.
    # This is the one case measured as a WHOLE PROCESS: a control window opened
    # inside main() would start after the loader had already run.
    Case("boot", "boots", 1, whole_process=True),
    # cursor-step. One mt_next, which is one metta_c_next plus the
    # decode of its answer into a C atom and the render of its text. DECIDED BY
    # instructions:u AND CPU TIME, and this case is the counter rule in one
    # number: the engine retires 10 inferences per answer while the process
    # retires about 17,400 instructions, so what the inference counter can see
    # is a rounding error on what the step costs. Its pin still earns its place
    # -- it catches a change in the engine's per-answer reduction -- but it
    # cannot referee the C half at all.
    Case("cursor-step", "steps", 200_000),
    # term-in. A term crossing FROM C INTO the engine: mt_show_dup encodes a C
    # atom into a Prolog term and asks the engine to write it, the only public
    # door that crosses this way without also storing or evaluating something.
    # DECIDED BY instructions:u AND CPU TIME; the encode is pure C and retires
    # nothing, so the inference pin here prices only the writer on the far side.
    Case("term-in", "crossings", 60_000),
    # term-out. The mirror: mt_parse runs the engine's reader and decodes
    # the resulting Prolog term into a C atom. Same term, same text door,
    # opposite crossing, and the pair is what makes the two rows comparable.
    # DECIDED BY instructions:u AND CPU TIME, for the same reason.
    Case("term-out", "crossings", 60_000),
    # space-pair. Store one fact and retrieve it by its key, which is what a C
    # host does with a space. DECIDED BY instructions:u AND CPU TIME. The
    # inference pin is the most informative one in the suite, because both
    # doors are engine work: the add asserts and the match runs the engine's
    # own matcher, so a matcher change lands here first.
    Case("space-pair", "pairs", 20_000),
    # error-ball. An engine exception crossing back to C as words: the engine
    # raises, call_bridge copies the ball off the stacks with PL_record, and
    # render_ball asks metta_c_error_text/2 for its text. DECIDED BY
    # instructions:u AND CPU TIME. A failed assertion is the raiser because
    # MeTTa keeps most failures AS values, so nothing else reaches this path
    # [source: extensions/cetta/tests/test_cetta.c,
    # test_an_engine_error_reaches_c_as_words]. The engine also reports each
    # failure on stderr, and that report is inside the region on purpose: a C
    # host pays for it.
    Case("error-ball", "raises", 2_000),
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


def counter_configuration() -> dict[str, bool | list[str]]:
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
    """
    return {
        "c_reader": (ROOT / "engine" / "reader.so").is_file(),
        "c_writer": (ROOT / "engine" / "writer.so").is_file(),
        "c_json": (ROOT / "engine" / "json_codec.so").is_file(),
        "seats": loaded_seats(),
    }


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
) -> list[str]:
    """Observe every case on every counter, one message per failing counter.

    Each counter is compared SEPARATELY rather than in one try block. Stopping
    at the first would let an instruction regression hide a CPU regression on
    the same row, and the two only decide together: they are here precisely
    because each sees what the other cannot. It is the masking
    benchmarks/check_instructions.py was fixed for one level up, where it was
    one case hiding another.
    """
    failures: list[str] = []
    for case in cases:
        instructions, cpu, inferences = sample(case, rounds)
        outside: list[str] = []
        for observe in (
            partial(
                baseline.observe_counter,
                case.name,
                unit=case.unit,
                operations=case.operations,
                samples=inferences,
            ),
            partial(baseline.observe_measurement, case.name, INSTRUCTIONS, instructions),
            partial(baseline.observe_measurement, case.name, CPU_SECONDS, cpu),
        ):
            try:
                observe()
            except (AssertionError, KeyError) as error:
                outside.append(f"{case.name}: {error}")
        report = (
            f"{case.name}: instructions={list(instructions)} "
            f"cpu={list(cpu)} inferences={list(inferences)}"
        )
        #Both band directions land on the same tag, and so does a missing row,
        #so it names the outcome rather than one side of it.
        print(f"{report} OUTSIDE BAND" if outside else report)
        failures += outside
    return failures


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
    failures = observe_all(
        baseline, [BY_NAME[name] for name in arguments.cases], arguments.rounds
    )
    baseline.finish()
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
    print(f"{len(arguments.cases)} case(s) within band")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
