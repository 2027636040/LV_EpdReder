"""Build and run O6 host tests with GCC, ASan/UBSan and the real common parser.

Run from Linux or WSL: python3 modules/books/codecs/mobi/tests/run_o6.py
Use --measure 1 to exercise the optional per-pass decompressor/DFS summaries.
All generated sources, binaries, fixtures and logs stay in mobi/output/o6.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import argparse
import os
import re
import runpy
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--measure", type=int, choices=(0, 1), default=0)
    parser.add_argument("--baseline", default="870e5101114593223e850fa9b0f5a96c8a304d51")
    parser.add_argument("--allocation-checks", type=int, default=256)
    args = parser.parse_args()
    codec = Path(__file__).resolve().parent.parent
    books = codec.parents[1]
    repo = books.parents[1]
    vendor = books / "third_party/libmobi/src"
    formats = books / "formats"
    output = codec / "output/o6" / str(args.measure)
    output.mkdir(parents=True, exist_ok=True)
    sources = runpy.run_path(str(formats / "sources.py"))
    flags = [os.environ.get("CC", "gcc"), "-std=gnu11", "-O1", "-g",
             "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", "-fno-pie"]
    private_flags = flags + ["-DBOOK_MOBI", "-DRT_USING_DFS", "-DHAVE_STRDUP",
                             "-include", str(codec / "tests/compat.h")]
    if args.measure:
        private_flags += ["-DBOOK_MOBI_MEASURE=1"]
    includes = [codec / "tests", codec, vendor, formats,
                repo / "project/build_dpi-hdk_lb57gyd7n6_epd_hcpu"]
    private_flags += ["-I" + str(path) for path in includes]
    common_flags = flags + ["-DLIBXML_STATIC"] + [
        "-I" + path for path in sources["include_paths"](formats)]
    baseline = subprocess.check_output(
        ["git", "-C", str(repo), "show",
         f"{args.baseline}:modules/books/codecs/mobi/mobi_converter.c"])
    baseline_source = output / "baseline_converter.c"
    if not baseline_source.exists() or baseline_source.read_bytes() != baseline:
        baseline_source.write_bytes(baseline)
    jobs = []

    def add(source, name, options):
        obj = output / (name + ".o")
        jobs.append((Path(source), obj, options))
        return str(obj)

    common = [add(source, "common_" + Path(source).stem, common_flags)
              for source in sources["source_files"](formats)]
    names = ["buffer", "compression", "index", "memory", "meta", "parse_rawml",
             "read", "structure", "util"]
    private = [add(vendor / (name + ".c"), "vendor_" + name,
                   private_flags + (["-DMOBI_MEASURE_DECOMPRESSION"] if name == "util" else []))
               for name in names]
    private.append(add(codec / "mobi_port.c", "port", private_flags))
    current = add(codec / "mobi_converter.c", "converter", private_flags)
    old = add(baseline_source, "baseline", private_flags + [
        "-Dbook_converter=baseline_book_converter", "-Dbook_module_profile=baseline_module_profile"])
    targets = add(codec / "tests/o6_targets.c", "targets", private_flags)
    compare = add(codec / "tests/o6_compat.c", "compat", private_flags + ["-DTEST_COMMON_MARKUP"])
    regression = add(codec / "tests/regression.c", "regression", private_flags + ["-DTEST_COMMON_MARKUP"])
    compare_raw = add(codec / "tests/o6_compat.c", "compat_raw", private_flags)
    regression_raw = add(codec / "tests/regression.c", "regression_raw", private_flags)

    def compile_job(job):
        source, obj, options = job
        # Recompile private sources to account for included converter/stub headers.
        if obj.name.startswith("common_") and obj.exists() and obj.stat().st_mtime > source.stat().st_mtime:
            return ""
        result = subprocess.run(options + ["-c", str(source), "-o", str(obj)],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(result.stdout)
        return result.stdout

    with ThreadPoolExecutor(max_workers=4) as executor:
        logs = list(executor.map(compile_job, jobs))
    (output / "build.log").write_text("".join(logs))
    for name, objects in [("targets", [targets] + private),
                          ("compat", [compare, old, current] + private + common),
                          ("regression", [regression, current] + private + common),
                          ("compat_raw", [compare_raw, old, current] + private),
                          ("regression_raw", [regression_raw, current] + private)]:
        subprocess.run(flags + ["-no-pie"] + objects + ["-Wl,--gc-sections", "-o", str(output / name)], check=True)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def run(name, command):
        log = output / (name + ".log")
        with log.open("w") as stream:
            result = subprocess.run([str(path) for path in command], env=env,
                                    stdout=stream, stderr=subprocess.STDOUT)
        text = log.read_text(errors="replace")
        if result.returncode:
            raise RuntimeError(f"{name} exited {result.returncode}: {log}\n{text[-8000:]}")
        for line in text.splitlines():
            if line.startswith(("PASS", "baseline result=")):
                print(line, flush=True)
        return text

    print(f"baseline={args.baseline} measure={args.measure} sanitizers=address,undefined", flush=True)
    run("targets", [output / "targets"])
    samples = sorted((vendor.parent / "tests/samples").glob("*.mobi"))
    samples += sorted((vendor.parent / "tests/samples").glob("*.fail"))
    # A minimal uncompressed PalmDOC container allows exact control of filepos
    # targets, comments and tags spanning the decompressed record boundary.
    body = bytearray(b"<html><body><p id='named'>begin</p>")
    body += b"x" * (4094 - len(body))
    body += b"<a filepos='1'>inside tag</a><!--<a filepos='999999999'>--><p>tail</p>"
    for i in range(800):
        body += f"<a filepos='{(i * 37) % 4094}'>link {i}</a>".encode()
    body += b"<a filepos='0'>duplicate</a><a href='#named'>named</a>"
    # Fixed-width formatting keeps the EOF target equal to the final byte length.
    suffix = b"<a filepos='%010d'>EOF</a></body></html>"
    body += suffix % (len(body) + len(suffix % 0))
    records = [struct.pack(">HHIHHHH", 1, 0, len(body), (len(body) + 4095) // 4096, 4096, 0, 0)]
    records += [body[i:i + 4096] for i in range(0, len(body), 4096)]
    header = bytearray(78)
    header[:8] = b"O6 links"
    header[60:68] = b"TEXtREAd"
    header[76:78] = struct.pack(">H", len(records))
    offset = 78 + len(records) * 8 + 2
    table = bytearray()
    for i, record in enumerate(records):
        table += struct.pack(">II", offset, i)
        offset += len(record)
    fixture = output / "dense-links.mobi"
    fixture.write_bytes(header + table + b"\0\0" + b"".join(records))
    samples.append(fixture)
    compat_log = run("compat", [output / "compat"] + samples)
    if args.measure:
        # The baseline has no phase selection; only the new converter can emit
        # nonzero phase-1/2 counts, including KF8 reconstruction inside libmobi.
        for phase in (1, 2):
            assert re.search(rf"measure phase={phase} decode_calls=[1-9]", compat_log)
            assert re.search(rf"measure phase={phase} .*read_calls=[1-9]", compat_log)
        print("PASS measurements: both passes reach actual decompressor and DFS counters", flush=True)
    else:
        assert "measure phase=" not in compat_log
        print("PASS baseline: decompressor/DFS measurement summaries remain disabled", flush=True)
    textread = vendor.parent / "tests/samples/sample-textread.mobi"
    run("compat-raw", [output / "compat_raw", textread, fixture])
    run("failure-textread", [output / "regression_raw", textread, args.allocation_checks])
    for name in ("sample-cp1252.mobi", "sample-ncx.mobi",
                 "sample-unicode-huffdic.mobi", "sample-obfuscated-fonts.mobi"):
        run("failure-" + name, [output / "regression", vendor.parent / "tests/samples" / name,
                               args.allocation_checks])
    run("failure-dense", [output / "regression", fixture, args.allocation_checks])
    print(f"PASS O6 host suite; logs and binaries: {output}", flush=True)


if __name__ == "__main__":
    main()
