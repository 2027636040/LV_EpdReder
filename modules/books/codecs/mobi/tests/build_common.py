"""Build the MOBI/common-markup integration harness with a Linux host compiler."""
from pathlib import Path
import os
import runpy
import subprocess

codec = Path(__file__).resolve().parent.parent
books = codec.parents[1]
repo = books.parents[1]
vendor = books / "third_party/libmobi/src"
formats = books / "formats"
listing = runpy.run_path(str(formats / "sources.py"))
output = codec / "output/host-linux"
output.mkdir(exist_ok=True, parents=True)
common = listing["source_files"](formats)
includes = listing["include_paths"](formats)
common_cmd = [os.environ.get("CC", "gcc"), "-std=gnu11", "-O1", "-g",
              "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
              "-fno-omit-frame-pointer", "-DLIBXML_STATIC"]
objects = []
for source in common:
    obj = output / ("common_" + Path(source).stem + ".o")
    if not obj.exists() or obj.stat().st_mtime < Path(source).stat().st_mtime:
        subprocess.run(common_cmd + ["-I" + p for p in includes] + ["-c", source, "-o", str(obj)], check=True)
    objects.append(str(obj))
names = ["buffer", "compression", "index", "memory", "meta", "parse_rawml", "read", "structure", "util"]
private = [str(codec / "tests/regression.c"), str(codec / "mobi_port.c"), str(codec / "mobi_converter.c")]
private += [str(vendor / (name + ".c")) for name in names]
command = common_cmd + ["-DBOOK_MOBI", "-DRT_USING_DFS", "-DHAVE_STRDUP", "-DTEST_COMMON_MARKUP",
                        "-include", str(codec / "tests/compat.h")]
command += ["-I" + str(p) for p in [codec / "tests", codec, vendor, formats,
    repo / "project/build_dpi-hdk_lb57gyd7n6_epd_hcpu"]]
subprocess.run(command + private + objects + ["-Wl,--gc-sections", "-o", str(output / "regression")], check=True)
