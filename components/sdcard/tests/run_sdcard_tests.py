"""Build the real component worker with deterministic RTOS/DFS/host substitutes."""

from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    package = tests.parent
    compiler = shutil.which("cl")
    if not compiler:
        raise SystemExit("Activate an MSVC developer environment.")
    with tempfile.TemporaryDirectory(prefix="sdcard-worker-") as output:
        for name in ("test_sdcard", "test_sdcard_port"):
            executable = Path(output) / f"{name}.exe"
            subprocess.run([
                compiler, "/nologo", "/std:c11", "/W4", "/WX", "/TC",
                "/D_CRT_SECURE_NO_WARNINGS",
                f"/I{tests / 'service_stubs'}", f"/I{package / 'include'}",
                f"/I{package / 'port'}", str(tests / f"{name}.c"),
                f"/Fe{executable}", f"/Fo{Path(output) / f'{name}.obj'}",
            ], cwd=output, check=True)
            subprocess.run([str(executable)], cwd=output, check=True)


if __name__ == "__main__":
    main()
