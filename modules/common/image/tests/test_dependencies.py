"""Check real application source graphs and license inputs after codec relocation."""

import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[4]
IMAGE = ROOT / "modules/common/image"
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("app_build_test_helpers", ROOT / "tools/tests/test_app_build.py")
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)
import native_apps


class SourceEnv:
    def __init__(self, sources):
        self.sources = sources

    def Clone(self):
        return SourceEnv(self.sources)

    def Append(self, **kwargs):
        pass

    def subst(self, value):
        return ""

    def Object(self, target, source):
        self.sources.append(Path(source).resolve())
        return [target]


class ImageDependencyTests(unittest.TestCase):
    def test_application_sconscript_sources_exist_without_gallery_codecs(self):
        for app in ("books", "weread", "gallery"):
            with self.subTest(app=app):
                sources = []
                env = SourceEnv(sources)

                def execute(path, **kwargs):
                    path = Path(path).resolve()
                    building = types.ModuleType("building")
                    building.GetCurrentDir = lambda: str(path.parent)
                    building.GetCurrentEnv = lambda: env
                    building.Glob = lambda pattern: [str(p) for p in path.parent.glob(pattern)]

                    def group(name, files, **kwargs):
                        sources.extend((path.parent / file).resolve() for file in files)
                        return list(files)

                    building.DefineGroup = group
                    script = types.ModuleType("SCons.Script")
                    script.Dir = lambda name: types.SimpleNamespace(abspath=str(path.parent))
                    scope = {"SConscript": execute, "Return": lambda name: None}
                    with patch.dict(sys.modules, {"building": building,
                                                  "SCons": types.ModuleType("SCons"),
                                                  "SCons.Script": script}):
                        exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), scope)
                    return scope["objects"]

                execute(ROOT / f"modules/{app}/SConscript")
                for source in sources:
                    self.assertTrue(source.is_file(), str(source))
                codecs = [p for p in sources if p.is_relative_to(IMAGE / "third_party")]
                self.assertEqual(len(codecs), 17)
                self.assertEqual(len(codecs), len(set(codecs)))
                if app != "gallery":
                    self.assertFalse(any(p.is_relative_to(ROOT / "modules/gallery") for p in sources))

    def test_native_package_tracks_shared_codecs_once(self):
        script = types.ModuleType("SCons.Script")
        script.Action = lambda action, description: action
        script.Copy = lambda target, source: (target, source)
        script.Value = lambda value: value
        with patch.dict(sys.modules, {"SCons": types.ModuleType("SCons"), "SCons.Script": script}):
            for app in ("books", "weread", "gallery"):
                with self.subTest(app=app):
                    env = helpers.BuildEnv(ROOT)
                    native_apps.add_package(env, ROOT / "SiFli-SDK", ROOT, app, {})
                    inputs = env.commands[0][1]
                    for name in ("libpng/png.c", "zlib/inflate.c", "tjpgd/tjpgd.c"):
                        self.assertEqual(inputs.count(str(IMAGE / "third_party" / name)), 1)
                    if app != "gallery":
                        self.assertFalse(any(Path(p).is_relative_to(ROOT / "modules/gallery")
                                             for p in inputs if isinstance(p, str)))

    def test_books_and_weread_license_files_exist(self):
        for builder in (native_apps.add_books_package, native_apps.add_weread_package):
            with self.subTest(builder=builder.__name__):
                with patch.object(native_apps, "add_package") as package:
                    builder({}, ROOT / "SiFli-SDK", ROOT)
                resources = package.call_args.kwargs["resources"]
                for name, path in resources.items():
                    self.assertTrue(path.is_file(), name)
                for name in ("libpng", "zlib"):
                    self.assertEqual(resources[f"licenses/{name}.txt"], IMAGE / "third_party" / name / "LICENSE")


if __name__ == "__main__":
    unittest.main()
