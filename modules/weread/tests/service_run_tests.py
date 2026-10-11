"""Run actual weread_service.c with deterministic platform/client/store/reader fakes.

Run under Linux/WSL with a host C compiler. No service source is generated or
rewritten. ASan/UBSan and the fake allocation ledger validate every scenario.
"""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="weread-service-") as directory:
        executable = Path(directory) / "service_test"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-g", "-O1",
                   "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-fsanitize=address,undefined",
                   "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                   "-I" + str(here / "service_include"),
                   str(here / "service_test.c"), "-o", str(executable)]
        subprocess.run(command, check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(executable)], cwd=directory, env=env, check=True, timeout=60)


if __name__ == "__main__":
    main()
