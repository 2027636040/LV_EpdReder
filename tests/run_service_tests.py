#!/usr/bin/env python3
"""Compile and run the real document service with host fault-injection stubs."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tests", nargs="*", help="Optional test names from --list")
    parser.add_argument("--list", action="store_true", help="List available tests")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"), help="C compiler (default: CC or gcc)")
    parser.add_argument("--sanitize", action="store_true", help="Enable AddressSanitizer and UndefinedBehaviorSanitizer")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="books-service-tests-") as build:
        executable = Path(build) / "service_test"
        command = shlex.split(args.cc) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_DEFAULT_SOURCE", "-Drt_kprintf=printf",
            "-Imodules/books/document/tests/include", "-Imodules/books", "-Isrc/services",
        ]
        if args.sanitize:
            command += ["-g", "-O1", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all"]
        command += ["modules/books/document/tests/service_test.c", "-o", str(executable)]
        print("Compile:", shlex.join(command), flush=True)
        subprocess.run(command, cwd=root, check=True)
        if args.list:
            return subprocess.run([str(executable), "--list"], cwd=build).returncode
        tests = args.tests or subprocess.check_output([str(executable), "--list"], cwd=build, text=True).splitlines()
        failures = []
        for test in tests:
            try:
                result = subprocess.run([str(executable), test], cwd=build, timeout=30)
                if result.returncode:
                    failures.append((test, result.returncode))
            except subprocess.TimeoutExpired:
                failures.append((test, "timeout after 30 seconds"))
        for test, code in failures:
            print(f"FAIL: {test} (exit {code})", flush=True)
        print(f"Service tests: {len(tests) - len(failures)}/{len(tests)} passed", flush=True)
        return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
