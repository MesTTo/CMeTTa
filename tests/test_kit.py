# Purpose: prove the C corpus driver preserves long JSON strings and rejects a
#   trailing escape before emitting any report bytes.
# Assumes: argv names the built driver and this worktree's ai-tmp directory.
# Guarantees: exits nonzero unless a source above 4 KiB round-trips exactly,
#   a surrogate pair decodes, and a malformed later source leaves stdout empty.
# Owns resources: one PID-named directory below the supplied scratch root,
#   removed on every exit.

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def require(condition: bool, claim: str) -> None:
    if not condition:
        raise AssertionError(claim)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_kit.py DRIVER AI_TMP", file=sys.stderr)
        return 1

    driver = Path(sys.argv[1]).resolve()
    scratch_root = Path(sys.argv[2]).resolve()
    scratch = scratch_root / f"cmetta-kit-test-{os.getpid()}"
    require(scratch.name.startswith("cmetta-kit-test-"), "unsafe scratch name")
    scratch.mkdir(parents=True, exist_ok=False)

    try:
        long_source = "!(+ 1 1)" + " " * 6000
        unicode_source = '!(quote "😀")'
        valid = scratch / "long-and-unicode.json"
        valid.write_text(
            json.dumps(
                {"programs": [
                    {"source": long_source},
                    {"source": unicode_source},
                ]},
                ensure_ascii=True,
            ),
            encoding="utf-8",
        )
        completed = subprocess.run(
            [str(driver), str(valid)], capture_output=True, text=True, check=False
        )
        require(completed.returncode == 0, completed.stderr)
        report = json.loads(completed.stdout)
        require(report["programs"][0]["source"] == long_source,
                "the driver truncated a source above 4 KiB")
        require(report["programs"][0]["answers"][0]["text"] == "2",
                "the long source did not execute intact")
        require(report["programs"][1]["source"] == unicode_source,
                "the driver did not combine a JSON surrogate pair")

        malformed = scratch / "late-lone-backslash.json"
        malformed.write_bytes(
            b'{"programs":[{"source":"!(+ 1 1)"},{"source":"broken'
            + bytes((0x5C,))
        )
        refused = subprocess.run(
            [str(driver), str(malformed)], capture_output=True, check=False
        )
        require(refused.returncode == 1,
                "a source ending in a lone backslash was accepted")
        require(refused.stdout == b"",
                "a malformed late source emitted a partial JSON report")
        require(b"unterminated escape in JSON string" in refused.stderr,
                "the malformed source did not name its unterminated escape")
    finally:
        shutil.rmtree(scratch)

    print("kit long-string and bounded-escape contracts ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
