"""Offline release preparation, malformed inputs and pinned-runtime regressions."""

from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest

import check_release


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="sdcard-release-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / "source"
        shutil.copytree(check_release.PACKAGE, self.package,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    def recipe(self, suffix):
        path = self.package / "conanfile.py"
        path.write_text(path.read_text(encoding="utf-8") + suffix, encoding="utf-8")

    def cli(self, *args):
        output = io.StringIO()
        with redirect_stdout(output):
            code = check_release.main(["--package-root", str(self.package), "--format", "json", *args])
        return code, json.loads(output.getvalue())

    def test_local_package_and_strict_release_have_distinct_results(self):
        self.recipe('\n    user = None\n    url = None\n    homepage = None\n')
        code, result = self.cli()
        self.assertEqual(code, 0)
        self.assertTrue(result["local_ready"])
        self.assertEqual(len(result["release_errors"]), 3)
        self.assertFalse(result["publication_verified"])
        self.assertEqual(self.cli("--strict-release")[0], 1)

    def test_filled_metadata_and_namespace_mismatch(self):
        # Syntax fixtures only; these values do not establish publication rights.
        self.recipe('\n    user = "test-maintainer"\n'
                    '    url = "https://github.com/test-maintainer/sdcard"\n'
                    '    homepage = "https://github.com/test-maintainer/sdcard"\n')
        code, result = self.cli("--strict-release", "--namespace", "test-maintainer")
        self.assertEqual(code, 0)
        self.assertTrue(result["release_metadata_ready"])
        self.assertFalse(result["publication_verified"])
        self.assertEqual(self.cli("--strict-release", "--namespace", "another-maintainer")[0], 1)

    def test_placeholder_and_malformed_metadata(self):
        for value in (None, "", "<publisher>", "namespace", "todo", "User Name", "user/channel"):
            with self.subTest(namespace=value):
                self.assertFalse(check_release.valid_namespace(value))
        for value in (None, "", "<Package homepage here>", "ftp://github.com/repo", "/local",
                      "https://example.com/repo", "https://pkg.example.org", "https://host.invalid",
                      "https://user:secret@github.com/repo", "https://github.com:bad/repo", "https://["):
            with self.subTest(url=value):
                self.assertFalse(check_release.valid_url(value))

    def test_missing_license_and_export_pattern(self):
        (self.package / "LICENSE").unlink()
        self.recipe('\n    exports_sources = ("src/*",)\n')
        code, result = self.cli()
        self.assertEqual(code, 1)
        self.assertIn("Missing or empty package file: LICENSE", result["local_errors"])
        self.assertIn("Missing export pattern: include/*", result["local_errors"])

    def test_unpinned_and_malformed_locks(self):
        for value in ({"version": "0.5", "python_requires": ["sf-pkg-base/1.0.0@sifli"]},
                      {"version": "0.5", "python_requires": ["sf-pkg-base/2.0.0@sifli#" + "a" * 32]},
                      [], {"version": "0.5", "python_requires": "not a list"}):
            with self.subTest(lock=value):
                (self.package / "conan.lock").write_text(json.dumps(value), encoding="utf-8")
                self.assertEqual(self.cli()[0], 1)

    def test_recipe_is_never_executed(self):
        sentinel = self.root / "unexpected"
        self.recipe(f'\nraise RuntimeError({str(sentinel)!r})\n')
        self.assertEqual(self.cli()[0], 0)
        self.assertFalse(sentinel.exists())
        (self.package / "conanfile.py").write_text("invalid syntax (", encoding="utf-8")
        self.assertEqual(self.cli()[0], 1)

    def test_read_only_check_and_runtime_drift(self):
        deployed = self.root / "deployed"
        shutil.copytree(self.package, deployed)
        (deployed / "conanmanifest.txt").write_text("fixture\n", encoding="utf-8")
        (deployed / "src/sdcard.o").write_bytes(b"generated object in a built deployment")
        (deployed / "port/sdcard_sifli_sdio.obj").write_bytes(b"generated object")
        (self.package / "README.md").write_text("Updated release materials\n", encoding="utf-8")
        (self.package / "tests/new_check.py").write_text("# Additional package test\n", encoding="utf-8")

        def digest():
            return {p.relative_to(self.root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in self.root.rglob("*") if p.is_file()}

        before = digest()
        code, report = self.cli("--deployed-package", str(deployed))
        self.assertEqual(code, 0)
        self.assertEqual(report["deployed"]["runtime_mismatches"], [])
        self.assertEqual(report["deployed"]["material_differences"], ["README.md", "tests/new_check.py"])
        self.assertEqual(before, digest())
        with (self.package / "src/sdcard.c").open("a", encoding="utf-8") as output:
            output.write("\n/* Changed runtime */\n")
        code, report = self.cli("--deployed-package", str(deployed))
        self.assertEqual(code, 1)
        self.assertIn("src/sdcard.c", report["deployed"]["runtime_mismatches"])
        (self.package / "src/new.c").write_text("/* New runtime */\n", encoding="utf-8")
        self.assertIn("src/new.c", self.cli("--deployed-package", str(deployed))[1]
                      ["deployed"]["runtime_mismatches"])

    def test_invalid_deployment_and_missing_runtime(self):
        self.assertEqual(self.cli("--deployed-package", str(self.root / "missing"))[0], 1)
        (self.package / "src/sdcard.c").unlink()
        self.assertEqual(self.cli()[0], 1)


if __name__ == "__main__":
    unittest.main()
