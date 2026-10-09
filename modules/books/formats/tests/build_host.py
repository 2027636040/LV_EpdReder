"""Build the isolated converter harness with a POSIX host compiler."""
from pathlib import Path
import os
import runpy
import subprocess

root = Path(__file__).resolve().parent.parent
listing = runpy.run_path(str(root / "sources.py"))
build = root / "tests/build"
build.mkdir(exist_ok=True)
command = [os.environ.get("CC", "gcc"), "-std=c99", "-D_DEFAULT_SOURCE", "-DLIBXML_STATIC",
           "-g", "-O1", "-ffunction-sections", "-fdata-sections"]
if os.environ.get("SANITIZE"):
    command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
command += ["-I" + p for p in listing["include_paths"](root)]
object_dir = build / ("sanitized" if os.environ.get("SANITIZE") else "normal")
object_dir.mkdir(exist_ok=True)
headers = list(root.glob("*.h")) + list((root / "../third_party/libxml2").rglob("*.h")) + list((root / "../third_party/miniz").glob("*.h"))
header_time = max(p.stat().st_mtime for p in headers)
objects = []
for filename in listing["source_files"](root) + [str(root / "tests/host.c")]:
    source = Path(filename)
    obj = object_dir / source.with_suffix(".o").name
    if not obj.exists() or obj.stat().st_mtime < max(source.stat().st_mtime, header_time):
        subprocess.run(command + ["-c", str(source), "-o", str(obj)], check=True)
    objects.append(str(obj))
subprocess.run(command + objects + ["-Wl,--gc-sections", "-o", str(build / "book_formats_test")], check=True)
