"""Check the codec's environment switch without importing SCons or the SDK.

For diagnostic firmware builds in PowerShell, set $env:BOOK_MOBI_MEASURE = '1'
before invoking the normal module build. Run Remove-Item Env:BOOK_MOBI_MEASURE
before building the baseline again. Only the exact value '1' opts into counters;
the unset environment leaves the header's default of zero unchanged.
"""
import os
from pathlib import Path
import runpy
import sys
import types
import unittest
from unittest.mock import patch


class MeasureConfigTest(unittest.TestCase):
    def configure(self, value):
        groups, objects = [], []

        class Environment(dict):
            def Append(self, **kwargs):
                for key, item in kwargs.items():
                    self[key] = self.get(key, [] if isinstance(item, list) else "") + item

            def subst(self, value):
                return self[value.removeprefix("$")]

            def Object(self, target, source, **kwargs):
                objects.append((target, source, kwargs))
                return [target]

        env = Environment(CPPDEFINES=["EXISTING=7"], CCFLAGS="-mfpu=fpv5 -mfloat-abi=hard")
        building = types.ModuleType("building")
        codec = Path(__file__).resolve().parent.parent
        building.GetCurrentDir = lambda: str(codec)
        building.GetCurrentEnv = lambda: env
        building.Glob = lambda pattern: ["mobi_converter.c", "mobi_port.c"]
        building.Return = lambda name: None

        def define_group(name, sources, **kwargs):
            groups.append((name, sources, list(env["CPPDEFINES"]), kwargs))
            return sources

        building.DefineGroup = define_group
        with patch.dict(os.environ), patch.dict(sys.modules, {"building": building}):
            os.environ.pop("BOOK_MOBI_MEASURE", None)
            if value is not None:
                os.environ["BOOK_MOBI_MEASURE"] = value
            runpy.run_path(str(codec / "SConscript"))
        return env, groups, objects

    def test_baseline_unchanged_without_exact_opt_in(self):
        baseline = self.configure(None)
        self.assertEqual(baseline[0]["CPPDEFINES"], ["EXISTING=7"])
        for value in ("", "0", "yes", "true", "01", "1 "):
            with self.subTest(value=value):
                self.assertEqual(self.configure(value), baseline)

    def test_opt_in_reaches_codec_and_all_vendor_units(self):
        baseline_env, baseline_groups, baseline_objects = self.configure(None)
        env, groups, objects = self.configure("1")
        flag = "BOOK_MOBI_MEASURE=1"
        self.assertEqual(env["CPPDEFINES"], baseline_env["CPPDEFINES"] + [flag])
        self.assertEqual(groups[0][2], baseline_groups[0][2] + [flag])
        self.assertEqual(groups[0][3], baseline_groups[0][3])
        self.assertEqual(env["CCFLAGS"], baseline_env["CCFLAGS"])
        self.assertEqual(env["LINKFLAGS"], baseline_env["LINKFLAGS"])
        self.assertEqual(len(objects), 9)
        for actual, baseline in zip(objects, baseline_objects):
            self.assertEqual(actual[:2], baseline[:2])
            defines = actual[2]["CPPDEFINES"]
            self.assertEqual(defines.count(flag), 1)
            self.assertEqual([item for item in defines if item != flag], baseline[2]["CPPDEFINES"])
            self.assertEqual("MOBI_MEASURE_DECOMPRESSION" in defines,
                             Path(actual[1]).stem == "util")


if __name__ == "__main__":
    unittest.main(verbosity=2)
