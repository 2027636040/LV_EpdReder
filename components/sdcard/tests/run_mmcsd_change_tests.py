"""Compile and run the SDK core with RTOS/card stubs in an x86 MSVC prompt."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    sdk = Path(os.environ["SIFLI_SDK"])
    tests = Path(__file__).resolve().parent
    compiler = shutil.which("cl")
    if not compiler:
        raise SystemExit("Activate the x86 MSVC developer environment (vcvarsall.bat x86).")
    with tempfile.TemporaryDirectory(prefix="sdcard-mmcsd-") as output:
        executable = Path(output) / "mmcsd_change_tests.exe"
        subprocess.run([
            compiler, "/nologo", "/std:c11", "/W4", "/TC",
            "/D_CRT_SECURE_NO_WARNINGS",
            f"/I{tests / 'mmcsd_stubs'}",
            f"/I{sdk / 'rtos/rtthread/components/drivers/include'}",
            f"/I{sdk / 'rtos/rtthread/components/drivers/sdio'}",
            str(tests / "test_mmcsd_change.c"),
            f"/Fe{executable}", f"/Fo{Path(output) / 'mmcsd_change_tests.obj'}",
        ], cwd=output, check=True)
        subprocess.run([str(executable)], cwd=output, check=True)


if __name__ == "__main__":
    main()
