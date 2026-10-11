"""Link the real document service, cache, file observer and storage-record code."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[3]
    books = root / "modules/books"
    common = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
              "-DBOOK_FORMAT_TARGET", "-Wall", "-Wextra", "-Werror", "-g", "-O1", "-pthread",
              "-I" + str(here / "cache/include"), "-I" + str(here / "include"),
              "-I" + str(root / "src"), "-I" + str(books / "third_party/libxml2/include"),
              "-include", str(here / "cache/include/cache_io.h"),
              "-include", str(here / "cache_service_platform.h")]
    if args.sanitize:
        common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
    sources = [books / "document/document.c", books / "document/document_cache.c",
               books / "formats/book_fs.c", root / "src/services/storage_file.c",
               here / "cache/platform.c", here / "cache_service_test.c"]
    with tempfile.TemporaryDirectory(prefix="document-cache-service-") as directory:
        build = Path(directory)
        objects = []
        for source in sources:
            obj = build / (source.stem + ".o")
            command = common + (["-DCACHE_TEST_IO_IMPLEMENTATION"] if source.name == "platform.c" else [])
            if source.name in ("storage_file.c", "document.c"):
                command += ["-Wno-sign-compare"]
            subprocess.run(command + ["-c", str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        executable = build / "cache_service_test"
        wrapped = ["document_cache_observe_begin", "document_cache_observe_end", "document_cache_prepare",
                   "storage_file_commit", "document_cache_commit"]
        subprocess.run(common + objects + ["-Wl,--wrap=" + name for name in wrapped] +
                       ["-o", str(executable)], check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(executable), str(build)], env=env, check=True, timeout=60)


if __name__ == "__main__":
    main()
