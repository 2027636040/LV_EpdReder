"""Host regression tests for the production board model and SVG command consumer."""

import copy
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("preview_board", TOOLS / "preview_board.py")
preview = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preview)


class BoardPreviewTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="board-test-")
        cls.directory = Path(cls.temporary.name)
        try:
            cls.executable = preview.build_host(cls.directory)
        except subprocess.CalledProcessError as error:
            cls.temporary.cleanup()
            raise RuntimeError(str(error.stdout) + str(error.stderr)) from error
        cls.default_path = preview.BOARD / "res/layout.json"
        cls.default = json.loads(cls.default_path.read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_layout(self, value, width=684, height=1216, fail=None, json_fail=None):
        path = self.directory / "layout.json"
        path.write_bytes(value if isinstance(value, bytes) else json.dumps(value, ensure_ascii=False).encode("utf-8"))
        args = [str(self.executable), str(path), str(width), str(height)]
        if fail is not None or json_fail is not None:
            args.append(str(fail if fail is not None else 2**64 - 1))
        if json_fail is not None:
            args.append(str(json_fail))
        result = subprocess.run(args, capture_output=True, encoding="utf-8")
        if result.returncode in (0, 1):
            self.assertIn("live=0; json_live=0", result.stderr)
        self.assertNotEqual(result.returncode, 3, result.stderr)
        return result

    def test_default_geometry(self):
        result = self.run_layout(self.default)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(line) for line in result.stdout.splitlines()]
        panels = [c for c in commands if c["kind"] == "panel"]
        self.assertEqual([(p["x"], p["y"], p["width"], p["height"]) for p in panels],
                         [(24, 104, 636, 240), (24, 360, 310, 300), (350, 360, 310, 300), (24, 676, 636, 280)])
        for c in commands:
            self.assertGreater(c["width"], 0)
            self.assertGreater(c["height"], 0)
            self.assertLessEqual(c["x"] + c["width"], 684)
            self.assertLessEqual(c["y"] + c["height"], 1216)
        self.assertEqual(len(commands), 14)

    def test_reference_template_and_empty_board(self):
        commands = preview.commands(self.executable, preview.BOARD / "res/templates/reading.json")
        self.assertEqual(sum(c["kind"] == "panel" for c in commands), 3)
        self.assertEqual(self.run_layout({"version": 1, "title": "空看板", "cards": []}).returncode, 0)

    def test_svg_geometry_is_c_output(self):
        commands = preview.commands(self.executable, self.default_path)
        root = ET.fromstring(preview.svg(commands, 684, 1216))
        nodes = {node.attrib["id"]: node for node in root.iter() if "id" in node.attrib}
        for i, c in enumerate(commands):
            if c["kind"] == "panel":
                continue
            node = nodes[f"command-{i}"]
            for key in ("x", "y", "width", "height"):
                self.assertEqual(int(node.attrib[key]), c[key])
        self.assertIn("服务未安装", "".join(root.itertext()))

    def test_summary_copy_and_xml_escaping(self):
        commands = preview.commands(self.executable, self.default_path)
        root = ET.fromstring(preview.svg(commands, 684, 1216, {"weather": '<晴 & "暖">'}))
        self.assertIn('<晴 & "暖">', "".join(root.itertext()))
        root = ET.fromstring(preview.svg(commands, 684, 1216, {"weather": "a" * 200}))
        self.assertIn("a" * 95, "".join(root.itertext()))
        self.assertNotIn("a" * 96, "".join(root.itertext()))

    def test_bad_schema_and_json(self):
        malformed = [b"{", b"{}", b"[]", b"null", b'{"version":1,"title":"x","cards":[]} junk',
                     b'{"version":1,"version":1,"title":"x","cards":[]}',
                     b'{"version":1,"title":"x","cards":[]}\x00{}']
        for key, value in (("version", 2), ("title", 42), ("cards", {}), ("extra", True)):
            item = copy.deepcopy(self.default)
            item[key] = value
            malformed.append(item)
        for key, value in (("height", -1), ("height", 100.5), ("span", 0), ("summary", "../weather"),
                           ("text", None), ("unexpected", 1)):
            item = copy.deepcopy(self.default)
            item["cards"][0][key] = value
            malformed.append(item)
        for item in malformed:
            with self.subTest(item=str(item)[:90]):
                result = self.run_layout(item)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_geometry_overflow_and_bounds(self):
        changes = [("layout", "columns", 0), ("layout", "columns", 2**32 - 1),
                   ("layout", "margin", 2**32), ("layout", "gap", 2**32 - 1),
                   ("layout", "padding", 2**32 - 1), ("card", "height", 2**32),
                   ("card", "height", 2**32 - 1), ("card", "height", 1), ("card", "span", 3)]
        for group, key, value in changes:
            item = copy.deepcopy(self.default)
            (item["layout"] if group == "layout" else item["cards"][0])[key] = value
            with self.subTest(group=group, key=key, value=value):
                result = self.run_layout(item)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, "")
        self.assertEqual(self.run_layout(self.default, height=800).returncode, 1)
        self.assertEqual(self.run_layout(self.default, width=2**29).returncode, 2)

    def test_every_model_allocation_failure(self):
        ok = self.run_layout(self.default)
        count = int(re.search(r"; allocations=(\d+)", ok.stderr)[1])
        for fail in range(count):
            with self.subTest(allocation=fail):
                result = self.run_layout(self.default, fail=fail)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("allocation failed", result.stderr)
                self.assertEqual(result.stdout, "")

    def test_every_sdk_cjson_allocation_failure(self):
        ok = self.run_layout(self.default)
        count = int(re.search(r"json_allocations=(\d+)", ok.stderr)[1])
        self.assertGreater(count, 0)
        for fail in range(count):
            with self.subTest(allocation=fail):
                result = self.run_layout(self.default, json_fail=fail)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_actual_resources_without_count_or_text_cap(self):
        item = {"version": 1, "title": "资源测试", "layout": {"padding": 0, "gap": 0},
                "cards": [{"title": str(i), "text": "x" * 100000 if i == 0 else "", "height": 72} for i in range(70)]}
        result = self.run_layout(item, height=6000)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 212)

    def test_override_precedence(self):
        override = self.directory / "override.json"
        override.write_text("{}", encoding="utf-8")
        selected = preview.select_layout(self.default_path, override)
        self.assertEqual(selected, override)
        with self.assertRaises(subprocess.CalledProcessError):
            preview.commands(self.executable, selected)
        self.assertEqual(preview.select_layout(self.default_path, self.directory / "missing.json"), self.default_path)


if __name__ == "__main__":
    unittest.main()
