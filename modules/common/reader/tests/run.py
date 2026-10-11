"""Build real chapter converters/document/cache and run host lifecycle regressions."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import runpy
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]
    books = root / "modules/books"
    tests = books / "document/tests"
    formats = books / "formats"
    listing = runpy.run_path(str(formats / "sources.py"))
    flags = [os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-DBOOK_FORMAT_TARGET",
             "-DLIBXML_STATIC", "-DCACHE_REAL_FORMATS", "-g", "-O1", "-fno-pie", "-no-pie",
             "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra"]
    flags += ["-I" + str(p) for p in [tests / "cache/include", tests / "include", root / "src"]]
    flags += ["-I" + p for p in listing["include_paths"](formats)]
    flags += ["-include", str(tests / "cache_service_platform.h")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all"]
    sources = [here / "chapter_test.c", here / "api_namespace_test.c",
               here.parent / "reader_source.c", here.parent / "reader_stream.c",
               books / "document/document.c", books / "document/document_cache.c",
               root / "src/services/storage_file.c", tests / "cache/platform.c"]
    sources += [Path(p) for p in listing["source_files"](formats) if Path(p).name != "book_stream.c"]
    with tempfile.TemporaryDirectory(prefix="shared-reader-") as directory:
        build = Path(directory)
        def compile_source(source):
            extra = []
            if source.name in ("document.c", "document_cache.c", "storage_file.c", "book_fs.c", "platform.c"):
                extra += ["-include", str(tests / "cache/include/cache_io.h")]
            if source.name == "platform.c":
                extra += ["-DCACHE_TEST_IO_IMPLEMENTATION"]
            if "third_party" in str(source):
                extra += ["-w"]
            else:
                extra += ["-Werror", "-Wno-sign-compare"]
            obj = build / (source.stem + ".o")
            subprocess.run(flags + extra + ["-c", str(source), "-o", str(obj)], check=True)
            return str(obj)
        with ThreadPoolExecutor(max_workers=4) as workers:
            objects = list(workers.map(compile_source, sources))
        executable = build / "chapter_test"
        subprocess.run(flags + objects + ["-Wl,--gc-sections", "-o", str(executable)], check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(executable), str(build)], env=env, check=True, timeout=60)
        print("PASS API namespace: shared source and legacy TXT open/close link together")


if __name__ == "__main__":
    main()
