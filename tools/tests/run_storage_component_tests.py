"""Test the storage adapter and actual Launcher handoff function with substitutes."""

from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    repo = tests.parents[1]
    compiler = shutil.which("cl")
    if not compiler:
        raise SystemExit("Activate an MSVC developer environment.")
    with tempfile.TemporaryDirectory(prefix="storage-component-") as output:
        for enabled in (False, True):
            target = Path(output) / f"storage_{int(enabled)}"
            subprocess.run([
                compiler, "/nologo", "/std:c11", "/W4", "/WX", "/TC",
                "/D_CRT_SECURE_NO_WARNINGS",
                *([] if not enabled else ["/DPKG_USING_EPD_SDCARD"]),
                f"/I{tests / 'storage_stubs'}",
                f"/I{repo / 'components/sdcard/include'}",
                str(tests / "test_storage_component.c"),
                f"/Fo{target}.obj", f"/Fe{target}.exe",
            ], cwd=output, check=True)
            subprocess.run([f"{target}.exe"], cwd=output, check=True)
        # Extract this independent function verbatim; do not keep a second implementation.
        launcher = (repo / "src/ui/launcher.c").read_text(encoding="utf-8")
        start = launcher.index("bool launcher_storage_process(void)")
        opening = launcher.index("{", start)
        depth = 1
        end = opening + 1
        while depth:
            depth += (launcher[end] == "{") - (launcher[end] == "}")
            end += 1
        (Path(output) / "launcher_storage_under_test.h").write_text(
            launcher[start:end], encoding="utf-8")
        target = Path(output) / "launcher_storage"
        subprocess.run([
            compiler, "/nologo", "/std:c11", "/W4", "/WX", "/TC",
            f"/I{output}", str(tests / "test_launcher_storage.c"),
            f"/Fo{target}.obj", f"/Fe{target}.exe",
        ], cwd=output, check=True)
        subprocess.run([f"{target}.exe"], cwd=output, check=True)


if __name__ == "__main__":
    main()
