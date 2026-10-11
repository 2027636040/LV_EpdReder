import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("doc_build", Path(__file__).parents[1] / "build.py")
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class BundleTests(unittest.TestCase):
    def test_real_catalog_complete_and_deterministic(self):
        first = builder.collect(builder.ROOT)
        self.assertEqual(builder.serialize(first), builder.serialize(builder.collect(builder.ROOT)))
        self.assertEqual(len(first["documents"]), len({d["id"] for d in first["documents"]}))
        self.assertIn("modules/words/WDB2.md", {d["id"] for d in first["documents"]})
        for doc in first["documents"]:
            original = (builder.ROOT / doc["id"]).read_text(encoding="utf-8-sig")
            self.assertEqual(original, doc["markdown"])

    def test_script_payload_is_data(self):
        output = builder.serialize({"text": "</script><script>alert(1)</script>\u2028\u2029"})
        self.assertNotIn("</script>", output)
        self.assertIn("\\u2028", output)
        parsed = json.loads(output.split("window.EPD_DOCS=", 1)[1].removesuffix(";\n"))
        self.assertEqual(parsed["text"], "</script><script>alert(1)</script>\u2028\u2029")

    def fixture(self, root, names):
        (root / "doc").mkdir()
        (root / "docs").mkdir()
        (root / "docs/a.md").write_text("# 中文\n", encoding="utf-8")
        catalog = {"title": "test", "groups": [{"title": "test", "description": "", "documents": [[n, n] for n in names]}]}
        (root / "doc/catalog.json").write_text(json.dumps(catalog), encoding="utf-8")

    def test_duplicate_missing_and_path_escape(self):
        for names, message in [
            (["docs/a.md", "docs/a.md"], "Duplicate"),
            ([], "Add documents"),
            (["../outside.md"], "Invalid"),
            (["/absolute.md"], "Invalid"),
            (["docs/a.txt"], "Invalid"),
            (["docs/missing.md"], None),
        ]:
            with self.subTest(names=names), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                self.fixture(root, names)
                if message:
                    with self.assertRaisesRegex(ValueError, message): builder.collect(root)
                else:
                    with self.assertRaises(FileNotFoundError): builder.collect(root)

    def test_bundle_current(self):
        self.assertEqual((builder.ROOT / "doc/assets/content.js").read_text(encoding="utf-8"),
                         builder.serialize(builder.collect(builder.ROOT)))


if __name__ == "__main__":
    unittest.main()
