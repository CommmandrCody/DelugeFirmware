#! /usr/bin/env python3
"""
CAN THIS MACHINE BUILD, BEFORE ANYTHING CLAIMS IT DID.

    ./dbt doctor

Cody: "so we can test the build environment before we even run unit and test
harnesses."

The firmware suite was dead for days and reported it as a compile failure,
which reads like broken code. It was neither code nor SDK - it was a linker
older than the SDK next to it. Nothing in the build said so, because nothing
in the build was ever asked "can you link?", only told to link and allowed to
fail in whatever words the SDK happened to produce.

So this asks first, and says so in one screen: every host toolchain on the
machine, whether it can genuinely compile AND link, which one the tests will
use, whether the embedded toolchain is present, and whether the test build
directory is in a state the build system will actually refresh.

Exit code is the contract: 0 = you can build, 1 = you cannot, and the reason
is on the screen rather than buried in a wall of SDK header errors.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

import host_toolchain
import util

GREEN, RED, YELLOW, DIM, OFF = "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[0m"


def argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="doctor",
        description="Check the build environment BEFORE building or testing.",
    )
    parser.group = "Development"
    return parser


def _mark(ok, warn=False):
    return (
        f"{YELLOW}WARN{OFF}"
        if warn
        else (f"{GREEN} OK {OFF}" if ok else f"{RED}FAIL{OFF}")
    )


def _line(mark, label, detail=""):
    print(f"  [{mark}] {label}" + (f"  {DIM}{detail}{OFF}" if detail else ""))


def check_host() -> tuple[bool, dict | None]:
    """The host toolchain that builds and runs the unit tests."""
    print("\nHOST TOOLCHAIN (builds and runs the unit tests)")
    results = host_toolchain.survey()
    for r in results:
        _line(_mark(r["ok"]), r["name"], r["detail"])
    chosen = host_toolchain.working(results)
    if chosen is None:
        print(f"\n  {RED}No host toolchain on this machine can link a program.{OFF}")
        print("  Install Xcode, or repair Command Line Tools:")
        print("    xcode-select --install")
        return False, None

    # Say it out loud when the default would have failed. This is the exact
    # situation that cost days: `cc` on PATH resolves to the broken one, so a
    # plain `cc`/`cmake` invocation outside dbt still fails, and the reason is
    # worth stating rather than quietly routing around.
    if chosen["name"] != "PATH":
        by_name = {r["name"]: r for r in results}
        if not by_name["PATH"]["ok"]:
            print(
                f"\n  {YELLOW}Note:{OFF} the default `cc` on PATH cannot link "
                f"({DIM}{by_name['PATH']['detail']}{OFF})."
            )
            print(f"  dbt uses {chosen['name']} instead. To fix it system-wide:")
            print("    sudo xcode-select -s /Applications/Xcode.app/Contents/Developer")
    print(f"\n  Tests will build with: {chosen['name']}  {DIM}{chosen['cxx']}{OFF}")
    return True, chosen


def check_embedded() -> bool:
    """The ARM toolchain that builds the actual firmware binary."""
    print("\nEMBEDDED TOOLCHAIN (builds deluge.bin)")
    gcc = None
    for p in Path("toolchain").glob("*/*/arm-none-eabi-gcc/bin/arm-none-eabi-gcc"):
        gcc = p
        break
    if gcc is None:
        _line(_mark(False), "arm-none-eabi-gcc", "not found under toolchain/")
        print("    Run: ./dbt  (it downloads the toolchain on first use)")
        return False
    try:
        v = subprocess.run(
            [str(gcc), "--version"],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        ).stdout.splitlines()[0]
    except (OSError, subprocess.SubprocessError, IndexError) as e:
        _line(_mark(False), "arm-none-eabi-gcc", str(e))
        return False
    _line(_mark(True), "arm-none-eabi-gcc", v)
    return True


def check_test_dir() -> bool:
    """Whether build/tests is in a state the build system will refresh.

    task-test.py configures ONLY when build/tests does not exist, so a
    directory left behind by a failed configure is never repaired - the next
    run dies on a missing build.ninja and says nothing about why. That half-
    written stump is invisible unless something looks for it, so this looks.
    """
    print("\nTEST BUILD DIRECTORY")
    d = Path("build/tests")
    if not d.exists():
        _line(_mark(True), "build/tests", "absent - will be configured fresh")
        return True
    if not list(d.glob("*.ninja")):
        _line(_mark(False, warn=True), "build/tests", "STUMP from a failed configure")
        print("    The build system only configures when this directory is ABSENT,")
        print("    so it will not repair itself. Remove it and re-run:")
        print(
            "      find build/tests -type f -delete && find build/tests -depth -type d -delete"
        )
        return True  # recoverable, and the instruction to recover is right here
    cache = d / "CMakeCache.txt"
    if cache.exists():
        cxx = next(
            (
                ln.split("=", 1)[1].strip()
                for ln in cache.read_text(errors="replace").splitlines()
                if ln.startswith("CMAKE_CXX_COMPILER:")
            ),
            "",
        )
        # A cache pinned to a compiler that no longer links is the "cached pass"
        # trap: it keeps building against a toolchain the machine has moved past.
        if cxx and Path(cxx).exists():
            # With the SDK that candidate is paired with. Probing without one
            # would fail to find libc++ headers and condemn a perfectly good
            # cache - the same false FAIL the survey itself had to be fixed for.
            sdk = next(
                (c[3] for c in host_toolchain.CANDIDATES if c[2] == cxx or c[1] == cxx),
                None,
            )
            ok, detail = host_toolchain._probe(cxx.replace("c++", "cc"), cxx, sdk)
            _line(_mark(ok, warn=not ok), "cached compiler", f"{cxx} - {detail}")
            if not ok:
                print(
                    "    This cache is stale. Remove build/tests and re-run ./dbt test."
                )
        else:
            _line(
                _mark(False, warn=True),
                "cached compiler",
                f"{cxx or 'unknown'} - missing",
            )
    else:
        _line(_mark(True), "build/tests", "configured")
    return True


def main() -> int:
    os.chdir(util.get_git_root())
    print(f"\n{DIM}Build environment check - {Path.cwd()}{OFF}")

    host_ok, _ = check_host()
    embedded_ok = check_embedded()
    check_test_dir()

    print()
    if host_ok and embedded_ok:
        print(f"  {GREEN}Ready.{OFF} ./dbt build release   ./dbt test\n")
        return 0
    if embedded_ok and not host_ok:
        # Worth separating: the firmware still builds, only the tests cannot -
        # which is exactly the state that let a bin ship with no coverage.
        print(f"  {YELLOW}Firmware can build, but the unit tests CANNOT.{OFF}")
        print("  Anything built now is untested. Fix the host toolchain above.\n")
        return 1
    print(f"  {RED}Not ready.{OFF} See above.\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
