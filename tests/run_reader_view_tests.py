"""Build and run isolated non-TXT layout, view and font tests on a POSIX host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    tests = root / "modules/books/document/tests"
    flags = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-O1",
             "-ffunction-sections", "-fdata-sections", "-fno-pie",
             "-I" + str(tests / "include"), "-I" + str(root / "src/ui")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer"]
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="reader-view-tests-") as directory:
        for name in ("layout", "view", "font"):
            exe = Path(directory) / name
            sources = [str(tests / (name + "_test.c"))]
            if name == "view":
                sources += [str(root / "modules/common/reader/tests/api_namespace_test.c")]
            subprocess.run(flags + sources + ["-Wl,--gc-sections", "-no-pie", "-o", str(exe)], check=True)
            subprocess.run([str(exe)], env=env, check=True)
            if name == "view":
                print("PASS API namespace: shared view and legacy TXT open/close link together")


if __name__ == "__main__":
    main()
