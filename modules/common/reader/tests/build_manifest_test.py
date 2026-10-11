"""Evaluate the shared SConscript graph without a firmware toolchain."""
import glob
from pathlib import Path
import re
import runpy
import sys
import tempfile
import types


def main():
    root = Path(__file__).resolve().parents[4]
    records = []
    contexts = []

    class Environment:
        def Clone(self):
            return Environment()

        def Append(self, **kwargs):
            pass

        def Object(self, target, source):
            output = Path(target)
            if not output.is_absolute():
                output = contexts[-1][1] / output
            records.append((Path(source).resolve(), output.resolve()))
            return [str(output)]

    env = Environment()

    def run_script(path, variant_dir, duplicate=0):
        contexts.append((Path(path).resolve().parent, Path(variant_dir).resolve()))
        try:
            return runpy.run_path(str(path))["objects"]
        finally:
            contexts.pop()

    def define_group(name, sources, **kwargs):
        objects = []
        for source in sources:
            objects += env.Object(name + "_" + Path(source).stem + ".o", source)
        return objects

    building = types.ModuleType("building")
    building.GetCurrentDir = lambda: str(contexts[-1][0])
    building.GetCurrentEnv = lambda: env
    building.Glob = lambda pattern: glob.glob(str(contexts[-1][0] / pattern))
    building.DefineGroup = define_group
    building.SConscript = run_script
    building.Return = lambda name: None
    sys.modules["building"] = building
    sys.modules["SCons"] = types.ModuleType("SCons")
    script = types.ModuleType("SCons.Script")
    script.Dir = lambda name: types.SimpleNamespace(abspath=str(contexts[-1][1] / name))
    sys.modules["SCons.Script"] = script
    with tempfile.TemporaryDirectory(prefix="reader-manifest-") as output:
        run_script(root / "modules/common/reader/SConscript", output)
    sources = [p for p, _ in records]
    targets = [p for _, p in records]
    assert len(sources) == len(set(sources)), "duplicate compiled source"
    assert len(targets) == len(set(targets)), "colliding object targets"
    assert all(p.is_file() for p in sources), "missing implementation source"
    names = {p.name for p in sources}
    required = {"reader_view.c", "reader_font.c", "reader_source.c", "reader_stream.c",
                "document.c", "document_cache.c", "document_image.c", "book_image.c",
                "formats.c", "book_fs.c", "book_xml.c", "book_markup.c", "parser.c",
                "miniz.c", "raster_io.c", "raster_png.c", "tjpgd.c", "pngread.c", "inflate.c"}
    assert not required - names, required - names
    assert not names & {"book_stream.c", "document_view.c", "document_font.c", "reader.c",
                        "reader_service.c", "ui_reader.c", "ui_bookshelf_data.c", "app_main.c"}
    header = (root / "modules/common/reader/reader.h").read_text(encoding="utf-8")
    api = re.findall(r"\b((?:epd_)?reader_\w+)\s*\(", header)
    assert len(api) == 21 and all(name.startswith("epd_reader_") for name in api), api
    assert not re.search(r"#\s*define\s+reader_\w+", header), "legacy API macro alias"
    print(f"PASS standalone SConscript: {len(sources)} unique sources; document/font/layout/XML/miniz/image present; no books UI/service")
    print("PASS API namespace: all 21 public functions use epd_reader_ without legacy aliases")


if __name__ == "__main__":
    main()
