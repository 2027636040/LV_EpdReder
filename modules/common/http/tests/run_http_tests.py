"""Compile actual HTTP/weather sources against deterministic network/TLS fakes.

Uses MSVC discovered with vswhere on Windows, or CC/gcc/clang on POSIX.
The repository's existing miniz supplies the real gzip decoder.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile


def compiler_environment():
    env = {name.upper() if os.name == "nt" else name: value for name, value in os.environ.items()}
    cc = env.get("CC") or shutil.which("cl") or shutil.which("cc") or shutil.which("gcc")
    if cc:
        return cc, env
    if os.name != "nt":
        raise RuntimeError("Set CC to a host C compiler")
    vswhere = Path(env["PROGRAMFILES(X86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = subprocess.check_output(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        text=True).strip()
    vcvars = Path(installation) / "VC/Auxiliary/Build/vcvars64.bat"
    setup = subprocess.check_output('cmd /d /s /c "vcvars64.bat >nul && set"',
                                    cwd=vcvars.parent, text=True)
    for line in setup.splitlines():
        name, separator, value = line.partition("=")
        if separator and name:
            env[name.upper()] = value
    cc = shutil.which("cl", path=env.get("PATH"))
    if not cc:
        raise RuntimeError("MSVC environment did not provide cl.exe")
    return cc, env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="ASan on MSVC; ASan/UBSan on GCC/Clang")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]
    cc, env = compiler_environment()
    msvc = Path(cc).name.lower() in ("cl", "cl.exe")
    includes = [here / "include", here.parent, root / "src/platform", root / "src/services/net",
                root / "src/thirdparty/miniz", root / "modules/weather"]
    sources = [here.parent / "epd_http.c", root / "modules/weather/weather_http.c",
               here / "fake_transport.c", here / "http_test.c", root / "src/thirdparty/miniz/miniz.c"]
    print(f"Host compiler: {cc}", flush=True)
    with tempfile.TemporaryDirectory(prefix="epd-http-test-") as directory:
        build = Path(directory)
        objects = []
        for source in sources:
            obj = build / (source.stem + (".obj" if msvc else ".o"))
            if msvc:
                command = [cc, "/nologo", "/std:c11", "/utf-8", "/Od", "/Zi", "/MD", "/D_CRT_SECURE_NO_WARNINGS",
                           "/DMINIZ_NO_ARCHIVE_APIS", "/DMINIZ_NO_STDIO"]
                command += ["/W0"] if source.name == "miniz.c" else ["/W4", "/WX"]
                command += ["/I" + str(p) for p in includes]
                if args.sanitize:
                    command += ["/fsanitize=address"]
                command += ["/c", str(source), "/Fo" + str(obj), "/Fd" + str(build / "compile.pdb")]
            else:
                command = [cc, "-std=c11", "-g", "-O1", "-DMINIZ_NO_ARCHIVE_APIS", "-DMINIZ_NO_STDIO",
                           "-DMINIZ_USE_UNALIGNED_LOADS_AND_STORES=0"]
                command += ["-Wall", "-Wextra", "-Werror"] if source.name != "miniz.c" else ["-w"]
                command += ["-I" + str(p) for p in includes]
                if args.sanitize:
                    command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                command += ["-c", str(source), "-o", str(obj)]
            subprocess.run(command, env=env, cwd=build, check=True)
            objects.append(str(obj))
        executable = build / ("http_test.exe" if msvc else "http_test")
        if msvc:
            command = [cc, "/nologo", *objects, "/Fe" + str(executable), "/link", "/DEBUG", "/INCREMENTAL:NO"]
        else:
            command = [cc, *objects, "-lm", "-o", str(executable)]
            if args.sanitize:
                command += ["-fsanitize=address,undefined"]
        subprocess.run(command, env=env, cwd=build, check=True)
        env.update(ASAN_OPTIONS=("halt_on_error=1" if msvc else "detect_leaks=1:halt_on_error=1"),
                   UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(executable)], env=env, cwd=build, check=True, timeout=60)


if __name__ == "__main__":
    main()
