"""Exercise the production cover/resource/book_image/raster PNG+JPEG pipeline.

Only HTTP, storage, platform and URI resolution are mocked. No network is used.
"""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[2]
image = root / "modules/common/image"
vendor = image / "third_party"
spec = importlib.util.spec_from_file_location("raster_fixture", image / "tests/test_raster.py")
fixtures = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(fixtures)
sources = [here / "covers_raster_test.c", root / "modules/books/image/book_image.c"]
sources += [image / name for name in ("raster_decode.c", "raster_io.c", "raster_png.c", "raster_jpeg.c", "raster_alloc.c")]
sources += [vendor / "libpng" / (name + ".c") for name in
            "png pngerror pngget pngmem pngread pngrio pngrtran pngrutil pngset pngtrans".split()]
sources += [vendor / "zlib" / (name + ".c") for name in "adler32 crc32 inflate inftrees inffast zutil".split()]
sources += [vendor / "tjpgd/tjpgd.c"]
includes = [here / "covers_include", image, root / "src/platform",
            vendor / "libpng", vendor / "zlib", vendor / "tjpgd"]
with tempfile.TemporaryDirectory(prefix="weread-covers-raster-") as temporary:
    directory = Path(temporary)
    output = directory / ("covers_raster.exe" if os.name == "nt" else "covers_raster")
    runs = []
    for sw, sh, channels, interlace in [(7, 3, 1, False), (80, 144, 3, False), (97, 61, 4, True)]:
        path = directory / f"{sw}-{sh}.png"
        pixel = fixtures.png(path, sw, sh, channels, interlace)
        w, h = 56, sh * 56 // sw
        if h > 72:
            h, w = 72, sw * 72 // sh
        if w > sw:
            w, h = sw, sh
        expected = directory / f"{sw}-{sh}.gray"
        expected.write_bytes(fixtures.expected(sw, sh, w, h, pixel))
        runs.append([str(output), str(path), str(expected), str(w), str(h)])
    fixture = (root / "SiFli-SDK/example/hal/jpegd/assets/100x100_jpeg.dat").read_text()
    jpeg = directory / "sample.jpg"
    jpeg.write_bytes(bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})\b", fixture)))
    runs.append([str(output), str(jpeg), "-", "56", "56"])
    if os.name == "nt":
        vcvars = os.environ.get("WEREAD_VCVARS", r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat")
        command = ["cl", "/nologo", "/std:c11", "/utf-8", "/W3", "/fsanitize=address", "/Zi", "/D_CRT_SECURE_NO_WARNINGS",
                   "/DZ_PREFIX", "/DPNG_ARM_NEON_OPT=0"]
        command += ["/I" + str(path) for path in includes] + [str(path) for path in sources] + ["/Fe:" + str(output)]
        line = '"' + vcvars + '" >nul && cd /d "' + temporary + '" && ' + subprocess.list2cmdline(command)
        line += " && " + " && ".join(subprocess.list2cmdline(run) for run in runs)
        subprocess.run(line, shell=True, check=True)
    else:
        command = [os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all",
                   "-DZ_PREFIX", "-DPNG_ARM_NEON_OPT=0"]
        command += ["-I" + str(path) for path in includes] + [str(path) for path in sources] + ["-lm", "-o", str(output)]
        subprocess.run(command, cwd=temporary, check=True)
        for run in runs:
            subprocess.run(run, cwd=temporary, check=True)
