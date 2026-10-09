"""Exercise application lifecycle and RAM snapshots with RTOS/UI substitutes."""

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
    tests = Path(__file__).resolve().parent
    repo = tests.parents[1]
    compiler = shutil.which("cl")
    if not compiler:
        raise SystemExit("Activate an MSVC developer environment.")
    with tempfile.TemporaryDirectory(prefix="app-storage-") as directory:
        output = Path(directory)
        for name, path in (
            ("service", "src/services/app_service.c"),
            ("card", "src/ui/platform/app_card.c"),
            ("module", "src/ui/platform/app_module.c"),
            ("memory", "src/ui/platform/app_memory_usage.c"),
        ):
            source = (repo / path).read_text(encoding="utf-8")
            # Only replace includes. All production state and functions remain intact.
            source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.MULTILINE)
            (output / f"{name}_under_test.h").write_text(source, encoding="utf-8")
        installer = (repo / "src/ui/platform/app_installer.c").read_text(encoding="utf-8")
        catalog = (repo / "src/ui/platform/app_catalog.c").read_text(encoding="utf-8")
        storage = (repo / "src/services/storage_file.c").read_text(encoding="utf-8")
        selected = "".join(function(installer, signature) for signature in (
            "static bool volume_ready_set(", "bool app_installer_volume_ready(",
            "bool app_installer_start("))
        selected += function(catalog, "bool app_catalog_release_storage(")
        selected += function(storage, "static bool app_location(")
        (output / "handoff_under_test.h").write_text(selected, encoding="utf-8")
        for name in ("service", "card", "handoff", "module", "memory"):
            target = output / name
            subprocess.run([
                compiler, "/nologo", "/std:c11", "/utf-8", "/W4", "/WX", "/TC",
                "/D_CRT_SECURE_NO_WARNINGS", f"/I{output}",
                str(tests / f"test_app_storage_{name}.c"),
                f"/Fo{target}.obj", f"/Fe{target}.exe",
            ], cwd=output, check=True)
            subprocess.run([str(target.with_suffix(".exe"))], cwd=output, check=True)


if __name__ == "__main__":
    main()
