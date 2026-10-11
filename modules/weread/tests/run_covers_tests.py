"""Run cover queue/resource tests without network, an account, RTOS or LVGL."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="weread-covers-") as temporary:
    output = Path(temporary) / ("covers_test.exe" if os.name == "nt" else "covers_test")
    if os.name == "nt":
        vcvars = Path(os.environ.get("WEREAD_VCVARS", r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"))
        command = ["cl", "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/fsanitize=address", "/Zi",
                   "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(here / "covers_include"),
                   str(here / "covers_test.c"), "/Fe:" + str(output)]
        subprocess.run('"' + str(vcvars) + '" >nul && cd /d "' + temporary + '" && ' +
                       subprocess.list2cmdline(command) + ' && "' + str(output) + '"', shell=True, check=True)
    else:
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all",
                   "-I" + str(here / "covers_include"), str(here / "covers_test.c"), "-o", str(output)]
        subprocess.run(command, cwd=temporary, check=True)
        subprocess.run([str(output)], cwd=temporary, check=True)
