"""Check the package declaration without building firmware or resolving remotes."""

import ast
import importlib.util
import os
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch


PACKAGE = Path(__file__).resolve().parents[1]
SDK = Path(os.environ["SIFLI_SDK"]) if os.environ.get("SIFLI_SDK") else None


def load_sdk_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PackageMetadataTests(unittest.TestCase):
    def test_recipe_metadata(self):
        tree = ast.parse((PACKAGE / "conanfile.py").read_text(encoding="utf-8"))
        recipe = next(node for node in tree.body if isinstance(node, ast.ClassDef))
        fields = {node.targets[0].id: ast.literal_eval(node.value)
                  for node in recipe.body if isinstance(node, ast.Assign)}
        self.assertEqual(fields["name"], "epd-sdcard")
        self.assertEqual(fields["version"], "1.0.0")
        self.assertEqual(fields["license"], "LicenseRef-SiFli")
        self.assertEqual(fields["python_requires_extend"], "sf-pkg-base.SourceOnlyBase")
        self.assertEqual(fields["support_sdk_version"], "2.5.0")
        self.assertNotIn("user", fields)
        self.assertIn("LICENSE", fields["exports_sources"])
        self.assertIn("conandata.yml", fields["exports_sources"])

    def test_scons_source_selection(self):
        source = (PACKAGE / "SConscript").read_text(encoding="utf-8")
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                calls = []
                building = types.ModuleType("building")
                building.GetCurrentDir = lambda: str(PACKAGE)
                building.GetDepend = lambda name: enabled
                building.DefineGroup = lambda *args, **kwargs: calls.append((args, kwargs))
                scope = {"Return": lambda name: None}
                with patch.dict(sys.modules, {"building": building}):
                    exec(compile(source, str(PACKAGE / "SConscript"), "exec"), scope)
                if not enabled:
                    self.assertEqual(calls, [])
                    self.assertEqual(scope["group"], [])
                else:
                    self.assertEqual(len(calls), 1)
                    args, kwargs = calls[0]
                    self.assertEqual(args[1], [str(PACKAGE / "src/sdcard.c"),
                                               str(PACKAGE / "port/sdcard_sifli_sdio.c")])
                    self.assertEqual(kwargs["depend"], ["PKG_USING_EPD_SDCARD"])


@unittest.skipUnless(SDK, "Set SIFLI_SDK to validate the SDK manifest and Kconfig")
class SdkContractTests(unittest.TestCase):
    def test_consumer_manifest(self):
        deps = load_sdk_module("sdcard_test_sf_pkg_deps",
                               SDK / "tools/sdk_py_actions/sf_pkg_deps.py")
        manifest = deps.load_manifest(str(PACKAGE / "example/consumer/sf-pkg.yaml"))
        self.assertEqual(manifest["requires"], ["epd-sdcard/1.0.0"])
        self.assertEqual(manifest["support_sdk_version"], "2.5.0")
        self.assertEqual(manifest["enable"], [])

    def test_kconfig_dependencies(self):
        kconfiglib = load_sdk_module("sdcard_test_kconfiglib",
                                    SDK / "tools/kconfig/kconfiglib.py")
        kconf = kconfiglib.Kconfig(str(PACKAGE / "tests/Kconfig"), warn_to_stderr=False)
        option = kconf.syms["PKG_USING_EPD_SDCARD"]
        self.assertEqual(option.str_value, "n")
        option.set_value("y")
        self.assertEqual(option.str_value, "y")
        for name in ("RT_USING_PIN", "RT_USING_SDIO", "BSP_USING_SD_LINE",
                     "RT_USING_DFS", "RT_USING_DFS_ELMFAT"):
            with self.subTest(dependency=name):
                kconf.syms[name].set_value("n")
                self.assertEqual(option.str_value, "n")
                kconf.syms[name].set_value("y")
                self.assertEqual(option.str_value, "y")
        kconf.syms["SDIO_CARD_MODE"].set_value("1")
        self.assertEqual(option.str_value, "n")
        kconf.syms["SDIO_CARD_MODE"].set_value("0")
        kconf.syms["SD_INSERT_DETECT_PIN"].set_value("-1")
        self.assertEqual(option.str_value, "n")
        kconf.syms["SD_INSERT_DETECT_PIN"].set_value("11")
        self.assertEqual(option.str_value, "y")
        self.assertEqual(kconf.warnings, [])


if __name__ == "__main__":
    unittest.main()
