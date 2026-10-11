"""Build and run standalone cache tests with a POSIX host C compiler."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[4]
    books = root / "modules/books"
    common = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
              "-DBOOK_FORMAT_TARGET", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
              "-I" + str(here / "include"), "-I" + str(here.parent / "include"),
              "-I" + str(root / "src"), "-I" + str(books / "third_party/libxml2/include"),
              "-include", str(here / "include/cache_io.h")]
    if args.sanitize:
        common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
    sources = [books / "document/document_cache.c", books / "formats/book_fs.c",
               root / "src/services/storage_file.c", here / "platform.c", here / "cache_test.c"]
    with tempfile.TemporaryDirectory(prefix="document-cache-") as directory:
        build = Path(directory)
        objects = []
        for source in sources:
            obj = build / (source.stem + ".o")
            command = common + (["-DCACHE_TEST_IO_IMPLEMENTATION"] if source.name == "platform.c" else [])
            if source.name == "storage_file.c":
                command += ["-Wno-sign-compare"]
            subprocess.run(command + ["-c", str(source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        executable = build / "cache_test"
        subprocess.run(common + objects + ["-o", str(executable)], check=True)
        subprocess.run([str(executable), str(build)], check=True)


if __name__ == "__main__":
    main()
