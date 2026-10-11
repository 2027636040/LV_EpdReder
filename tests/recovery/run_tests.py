"""Run recovery, catalog and settings control tests with an isolated host compiler."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cl", help="MSVC cl or GCC/Clang")
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if not compiler:
        raise SystemExit("Compiler unavailable; run from a compiler developer shell or use --cc.")
    tests = Path(__file__).resolve().parent
    root = tests.parents[1]
    with tempfile.TemporaryDirectory(prefix="epd-recovery-") as directory:
        output = Path(directory)
        # Replace dependencies only; compile all production state and functions.
        for name, path in (
            ("storage", "src/services/storage_file.c"),
            ("recovery", "src/ui/platform/app_recovery.c"),
            ("catalog", "src/ui/platform/app_catalog.c"),
        ):
            source = (root / path).read_text(encoding="utf-8")
            source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.MULTILINE)
            (output / f"{name}_under_test.h").write_text(source, encoding="utf-8")
        management = (root / "src/ui/apps/app_management.c").read_text(encoding="utf-8")
        declarations = management[management.index("#define APP_ROWS"):management.index("static void list_refresh(")]
        selected = declarations + function(management, "static bool open_dynamic_settings(")
        selected += function(management, "bool app_list_action(")
        (output / "management_under_test.h").write_text(selected, encoding="utf-8")
        includes = [output, tests / "include", root / "src/services", root / "src/ui", root / "src/platform"]
        msvc = Path(compiler).stem.lower() == "cl"
        for finsh, forced in ((False, False), (True, False), (False, True), (True, True)):
            target = output / f"recovery_finsh{int(finsh)}_forced{int(forced)}"
            if msvc:
                command = [compiler, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC",
                           "/D_CRT_SECURE_NO_WARNINGS", "/wd4267", "/wd4018",
                           "/wd4302", "/wd4311", "/wd4312", "/wd4701",
                           *[f"/I{path}" for path in includes]]
                # Existing storage/catalog boolean chains trigger MSVC C4701.
                command += [f"/DAPP_RECOVERY={int(forced)}"]
                if finsh:
                    command += ["/DRT_USING_FINSH"]
                command += [str(tests / "test_recovery.c"), f"/Fo{target}.obj", f"/Fe{target}.exe"]
                executable = target.with_suffix(".exe")
            else:
                command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                           "-Wno-sign-compare", "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
                           *[f"-I{path}" for path in includes]]
                command += [f"-DAPP_RECOVERY={int(forced)}"]
                if finsh:
                    command += ["-DRT_USING_FINSH"]
                command += [str(tests / "test_recovery.c"), "-o", str(target)]
                executable = target
            subprocess.run(command, cwd=output, check=True)
            subprocess.run([str(executable)], cwd=output, check=True, timeout=30)


if __name__ == "__main__":
    main()
