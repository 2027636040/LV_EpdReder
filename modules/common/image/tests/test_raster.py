"""Compile the shared raster adapter with host shims and test its admission/cleanup.

Run with Python 3 and GCC on Linux (including WSL). No image packages required.
"""
from pathlib import Path
import os
import re
import struct
import subprocess
import tempfile
import zlib

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parents[3]
IMAGE = TESTS.parent
VENDOR = ROOT / "modules/gallery/third_party"


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(path, width, height, channels=1, interlaced=False, constant=None):
    def pixel(x, y):
        if constant is not None:
            return bytes([constant] * channels)
        gray = (x * 31 + y * 17) % 256
        return bytes([gray, gray, gray, (x * 7 + y * 11) % 256][:channels])

    compressor = zlib.compressobj()
    parts = []
    passes = [(0, 0, 1, 1)] if not interlaced else [
        (0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
        (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
    for x0, y0, dx, dy in passes:
        if x0 >= width or y0 >= height:
            continue
        flat = pixel(0, 0) * len(range(x0, width, dx)) if constant is not None else None
        for y in range(y0, height, dy):
            row = flat if flat is not None else b"".join(pixel(x, y) for x in range(x0, width, dx))
            parts.append(compressor.compress(b"\0" + row))
    parts.append(compressor.flush())
    header = struct.pack(">IIBBBBB", width, height, 8, {1: 0, 3: 2, 4: 6}[channels],
                         0, 0, int(interlaced))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                     chunk(b"IDAT", b"".join(parts)) + chunk(b"IEND", b""))
    return pixel


def expected(width, height, out_w, out_h, pixel):
    sums = [0] * (out_w * out_h)
    counts = [0] * len(sums)
    for y in range(height):
        for x in range(width):
            p = pixel(x, y)
            gray = p[0] if len(p) < 3 else (77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8
            if len(p) == 4:
                gray = (gray * p[3] + 255 * (255 - p[3]) + 127) // 255
            i = (y * out_h // height) * out_w + x * out_w // width
            sums[i] += gray
            counts[i] += 1
    return bytes((s + n // 2) // n for s, n in zip(sums, counts))


def main():
    with tempfile.TemporaryDirectory(prefix="raster-test-") as directory:
        directory = Path(directory)
        exe = directory / "raster"
        sources = [IMAGE / name for name in ("raster_decode.c", "raster_io.c", "raster_png.c",
                                             "raster_jpeg.c", "raster_alloc.c")]
        sources += [VENDOR / "libpng" / (name + ".c") for name in
                    "png pngerror pngget pngmem pngread pngrio pngrtran pngrutil pngset pngtrans".split()]
        sources += [VENDOR / "zlib" / (name + ".c") for name in
                    "adler32 crc32 inflate inftrees inffast zutil".split()]
        sources += [VENDOR / "tjpgd/tjpgd.c", TESTS / "raster_host.c"]
        command = [os.environ.get("CC", "gcc"), "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                   "-DZ_PREFIX", "-DPNG_ARM_NEON_OPT=0"]
        for include in (TESTS / "include", IMAGE, ROOT / "src/platform",
                        VENDOR / "libpng", VENDOR / "zlib", VENDOR / "tjpgd"):
            command += ["-I", str(include)]
        subprocess.run(command + [str(s) for s in sources] + ["-lm", "-o", str(exe)], check=True)

        def decode(path, width, height, failure=0, cancel=False):
            out = directory / "gray.raw"
            run = subprocess.run([str(exe), str(path), str(width), str(height), str(out),
                                  str(failure), str(int(cancel))], capture_output=True, text=True)
            assert run.returncode == 0, run.stderr
            assert "runtime error:" not in run.stderr, run.stderr
            ok, w, h, calls, peak = map(int, run.stdout.split())
            return ok, w, h, calls, peak, out.read_bytes() if ok else b""

        for channels in (1, 3, 4):
            for interlace in (False, True):
                path = directory / "small.png"
                pixel = png(path, 97, 61, channels, interlace)
                ok, w, h, calls, _, data = decode(path, 23, 17)
                assert ok and data == expected(97, 61, w, h, pixel)
                for failure in range(1, calls + 1):
                    assert not decode(path, 23, 17, failure)[0], failure
                assert not decode(path, 23, 17, cancel=True)[0]
        print("PASS: grayscale/RGB/RGBA, row/Adam7 pixels, allocation failures and cancellation")

        path = directory / "wide.png"
        png(path, 280000, 1, 4, constant=255)
        ok, w, h, _, peak, data = decode(path, 620, 966)
        assert ok and data == b"\xff" * (w * h) and peak > 1024 * 1024
        print(f"PASS: 280000-pixel source width, >1 MiB allocation; peak={peak}")

        path = directory / "large.png"
        png(path, 8193, 8193, constant=255)
        ok, _, _, _, _, data = decode(path, 1, 1)
        assert ok and data == b"\xff"
        print("PASS: >64 Mi pixels and 64-bit luminance accumulator")

        source = ROOT / "SiFli-SDK/example/hal/jpegd/assets/100x100_jpeg.dat"
        fixture = re.sub(r"/\*.*?\*/", "", source.read_text(), flags=re.S)
        path = directory / "sample.jpg"
        path.write_bytes(bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})\b", fixture)))
        ok, w, h, calls, _, data = decode(path, 23, 23)
        assert ok and w == h == 23 and len(data) == 529
        for failure in range(1, calls + 1):
            assert not decode(path, 23, 23, failure)[0]
        path.write_bytes(path.read_bytes()[:100])
        assert not decode(path, 23, 23)[0]
        print("PASS: baseline software JPEG, allocation failures and truncated input")


if __name__ == "__main__":
    main()
