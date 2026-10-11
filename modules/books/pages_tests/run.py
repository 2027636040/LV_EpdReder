"""Run the real pages.c scheduling/lifecycle functions with isolated host mocks."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"^(?:static )?(?:void|bool) " + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise AssertionError("Missing function: " + name)
    depth = 1
    # Ignore braces inside comments and string/character literals.
    tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
                         source[match.end():], re.S)
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
        if not depth:
            return source[match.start():match.end() + token.end()] + "\n"
    raise AssertionError("Unclosed function: " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    books = tests.parent
    source = (books / "pages.c").read_text(encoding="utf-8")
    header = (books / "books_ui.h").read_text(encoding="utf-8")
    names = ("reader_resume", "reader_resume_prepare", "reader_start", "reader_foreground",
             "reader_pause", "reader_stop", "reader_action", "books_process",
             "reader_status_render", "reader_footer_render", "reader_render", "reader_update",
             "reader_option_render", "reader_save_position")
    selected = "\n".join(function(source, name) for name in names)
    assert not re.search(r"\bui_reader_(?:process|index_process|prepare_process)\s*\(", source)
    assert len(re.findall(r"\bui_reader_step\s*\(", source)) == 1
    assert "ui_reader_step(8, foreground, idle)" in function(source, "books_process")
    declarations = source[source.index("bool reader_session;"):source.index("void bookshelf_start(")]
    types = header[header.index("#define WIDTH"):header.index("extern books_page_t *current;")]
    flags = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-variable", "-fno-pie", "-no-pie", "-I" + str(tests / "include"),
             "-I" + str(books), "-I" + str(books.parents[1] / "src/ui")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="books-pages-tests-") as directory:
        output = Path(directory)
        (output / "pages_types.h").write_text(types, encoding="utf-8")
        (output / "pages_under_test.h").write_text(declarations + selected, encoding="utf-8")
        target = output / "pages_test"
        subprocess.run(flags + ["-I" + directory, str(tests / "pages_test.c"), "-o", str(target)], check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(target)], env=env, check=True)


if __name__ == "__main__":
    main()
