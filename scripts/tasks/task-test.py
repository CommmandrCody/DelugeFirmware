#! /usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys

import host_toolchain
import util


def argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="test", description="Run deluge tests")
    parser.group = "Development"
    parser.add_argument(
        "-n",
        "--no-run",
        action="store_true",
        help="Only build the tests, without running them.",
    )

    return parser


def cmake_build() -> int:
    cmake_args = ["cmake"]
    cmake_args += ["--build", "build/tests/"]

    return subprocess.run(cmake_args, env=os.environ).returncode


def cmake_test() -> int:
    cmake_args = ["ctest"]
    cmake_args += ["--test-dir", "build/tests", "-C", "Debug"]

    return subprocess.run(
        cmake_args, env=dict(os.environ, CTEST_OUTPUT_ON_FAILURE="1")
    ).returncode


def cmake_configure() -> int:
    cmake_args = ["cmake"]
    cmake_args += ["-S", "tests/"]
    cmake_args += ["-B", "build/tests"]
    cmake_args += ["-G", "Ninja Multi-Config"]  # generator

    # CONFIGURE WITH A COMPILER THAT CAN ACTUALLY LINK, not whichever one `cc`
    # happens to name. On macOS `cc` resolves through xcode-select, which can
    # point at a Command Line Tools install whose linker is older than the SDK
    # beside it; every link then fails inside SDK headers and the suite reports
    # a compile failure that looks like broken code. See host_toolchain.py.
    env = os.environ
    chosen = host_toolchain.working()
    if chosen is None:
        print(
            "No host toolchain on this machine can compile and link a program.\n"
            "Run './dbt doctor' for the details.",
            file=sys.stderr,
        )
        return 1
    if chosen["name"] != "PATH":
        print(f"Using the {chosen['name']} toolchain for the host tests.")
    env = host_toolchain.env_for(chosen)

    return subprocess.run(cmake_args, env=env).returncode


def main() -> int:
    (args, unknown_args) = argparser().parse_known_args()

    os.chdir(util.get_git_root())

    # A directory left behind by a FAILED configure is not a configured build.
    # The original guard was `not os.path.exists("build/tests")`, so a stump -
    # a CMakeCache and nothing else - was treated as ready, and every later run
    # died on a missing build.ninja without ever saying why. Ask whether the
    # generator actually produced something instead of whether a path exists.
    configured = os.path.isdir("build/tests") and any(
        f.endswith(".ninja") for f in os.listdir("build/tests")
    )
    if not configured:
        if os.path.isdir("build/tests"):
            print("build/tests is a stump from a failed configure; reconfiguring.")
            shutil.rmtree("build/tests")
        result = cmake_configure()
        if result != 0:
            return result

    build = cmake_build()
    if build != 0 or args.no_run:
        return build

    return cmake_test()


if __name__ == "__main__":
    sys.exit(main())
