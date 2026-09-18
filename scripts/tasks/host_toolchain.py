"""
WHICH HOST COMPILER CAN ACTUALLY BUILD AND LINK A PROGRAM ON THIS MACHINE.

This exists because the answer changes underneath us and nothing reports it.

The tests build for the HOST, not the Deluge. On macOS that means whatever
`cc` resolves to, and on this machine `xcode-select` points at Command Line
Tools, whose linker is older than the SDK shipped beside it. Current SDKs
declare an `arm64e.x1` slice that the older `ld` cannot parse, so EVERY link
fails with:

    ld: tapi error: malformed file
    .../libSystem.B.tbd:4:20: error: unknown architecture arm64e.x1-macos

It reads like a broken SDK. It is not - the SDK is fine and Xcode's own,
newer linker reads it without complaint. The broken half is the linker.

That distinction matters because the previous fix recorded for this machine
was "export SDKROOT to an older Xcode SDK", which worked only for as long as
an older SDK existed. Xcode updated, the older SDK was removed, and the fix
silently stopped applying - leaving the firmware suite unable to compile at
all, which read as a RED suite rather than as no suite. A harness that cannot
run is not a failing harness; it is no coverage, and it looks the same.

So: stop naming a toolchain and stop naming an SDK. PROBE. Compile and link a
real program with each candidate and believe whichever one works, because
that is the only claim that stays true when Apple ships something new.
"""

import os
import subprocess
import tempfile
from pathlib import Path

# In preference order. Xcode's toolchain first: it is the one that gets updated
# alongside the SDKs, so when the two disagree it is the one that is current.
# Each entry also names its SDK, because the probe cannot rely on clang finding
# one for itself. dbt rewrites PATH to its own toolchain, so `xcrun` - which is
# how clang locates an SDK when none is given - is not reachable from inside a
# dbt task. Without an explicit -isysroot the probe fails with "'future' file
# not found" on a machine where the compiler is in fact perfectly good: a false
# FAIL, which in a health check is worse than no check at all.
XCODE_DEV = "/Applications/Xcode.app/Contents/Developer"
XCODE_BIN = f"{XCODE_DEV}/Toolchains/XcodeDefault.xctoolchain/usr/bin"
CLT = "/Library/Developer/CommandLineTools"

CANDIDATES = [
    (
        "Xcode",
        f"{XCODE_BIN}/cc",
        f"{XCODE_BIN}/c++",
        f"{XCODE_DEV}/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
    ),
    (
        "CommandLineTools",
        f"{CLT}/usr/bin/cc",
        f"{CLT}/usr/bin/c++",
        f"{CLT}/SDKs/MacOSX.sdk",
    ),
    # Whatever is on PATH. Last, because it is usually a shim for one of the
    # above and tells us nothing new - but on a machine with neither of those
    # (a CI runner, Linux) it is the only right answer. No SDK: on Linux there
    # is none, and on macOS letting it resolve its own is the honest test of
    # what a plain `cc` would do.
    ("PATH", "cc", "c++", None),
]


def _probe(cc: str, cxx: str, sdk: str | None = None) -> tuple[bool, str]:
    """Compile AND LINK a trivial program. Linking is the whole point.

    Compiling alone would have passed on this machine and told us nothing: the
    failure is in `ld`, which a compile-only check never reaches.
    """
    if os.path.sep in cc and not Path(cc).exists():
        return False, "not installed"
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "probe.cpp"
        # <future> pulls in libc++'s compressed_pair, which is where the newer
        # SDK headers trip the older toolchain. Including it means the probe
        # exercises the same headers the specs do rather than a trivial subset.
        src.write_text("#include <future>\nint main() { return 0; }\n")
        cmd = [cxx, str(src), "-std=c++20", "-o", str(Path(tmp) / "probe")]
        if sdk and Path(sdk).exists():
            cmd += ["-isysroot", sdk]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.SubprocessError) as e:
            return False, str(e)
        if r.returncode == 0:
            return True, "compiles and links"
        first = next(
            (ln for ln in r.stderr.splitlines() if "error:" in ln or "ld:" in ln),
            r.stderr.strip().splitlines()[0] if r.stderr.strip() else "failed",
        )
        return False, first.strip()


def survey() -> list[dict]:
    """Every candidate and what it can actually do. For reporting."""
    out = []
    for name, cc, cxx, sdk in CANDIDATES:
        ok, detail = _probe(cc, cxx, sdk)
        out.append(
            {"name": name, "cc": cc, "cxx": cxx, "sdk": sdk, "ok": ok, "detail": detail}
        )
    return out


def working(results: list[dict] | None = None) -> dict | None:
    """The first candidate that builds a program, or None if this Mac cannot."""
    for r in results if results is not None else survey():
        if r["ok"]:
            return r
    return None


def env_for(chosen: dict, base: dict | None = None) -> dict:
    """CC/CXX for a cmake configure.

    Only set when the user has not chosen for themselves: an explicit CC in the
    environment is a decision, and silently overriding it would make this file
    the thing that lies.
    """
    env = dict(base if base is not None else os.environ)
    env.setdefault("CC", chosen["cc"])
    env.setdefault("CXX", chosen["cxx"])
    # And the SDK that compiler was PROVEN against, for the same reason the
    # probe needed it: cmake runs under dbt's PATH too, so it cannot ask xcrun.
    if chosen.get("sdk") and Path(chosen["sdk"]).exists():
        env.setdefault("SDKROOT", chosen["sdk"])
    return env
