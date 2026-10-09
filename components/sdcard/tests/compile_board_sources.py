"""Compile component and demo using a real board's storage.c compile command.

This does not enable a second storage owner or link/overwrite the firmware.
"""

import argparse
import ctypes
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def windows_arguments(command):
    argc = ctypes.c_int()
    parse = ctypes.windll.shell32.CommandLineToArgvW
    parse.argtypes = (ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int))
    parse.restype = ctypes.POINTER(ctypes.c_wchar_p)
    result = parse(command, ctypes.byref(argc))
    if not result:
        raise ctypes.WinError()
    try:
        return [result[index] for index in range(argc.value)]
    finally:
        free = ctypes.windll.kernel32.LocalFree
        free.argtypes = (ctypes.c_void_p,)
        free.restype = ctypes.c_void_p
        free(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compile_commands", type=Path)
    args = parser.parse_args()
    entries = json.loads(args.compile_commands.read_text(encoding="utf-8"))
    entry = next(item for item in entries
                 if item["file"].replace("\\", "/").endswith("/services/storage.c"))
    command = windows_arguments(entry["command"])
    compiler = shutil.which(command[0])
    if not compiler:
        raise SystemExit("Activate the SDK ARM GCC environment.")
    command[0] = compiler
    output_index = command.index("-o") + 1
    source_index = command.index(entry["file"])
    package = Path(__file__).resolve().parents[1]
    command += [f"-I{package / 'include'}", f"-I{package / 'port'}", "-Wall", "-Wextra", "-Werror"]
    with tempfile.TemporaryDirectory(prefix="sdcard-arm-") as output:
        objects = []
        for source in ("src/sdcard.c", "port/sdcard_sifli_sdio.c", "example/sdcard_demo.c"):
            obj = Path(output) / (Path(source).stem + ".o")
            command[output_index] = str(obj)
            command[source_index] = str(package / source)
            subprocess.run(command, cwd=entry["directory"], check=True)
            print(f"ARM compile passed: {source}")
            objects.append(str(obj))
        size = Path(compiler).with_name("arm-none-eabi-size.exe")
        subprocess.run([str(size), *objects], check=True)


if __name__ == "__main__":
    main()
