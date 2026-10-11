"""Validate content-handler manifests and their preservation in installed packages."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('package_app', ROOT / 'tools/package_app.py')
package_app = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package_app)


class ContentManifest(unittest.TestCase):
    def test_validation(self):
        for value in (None, True, 1, 'txt', {}, [None], [1], [''], ['.txt'], ['TXT'],
                      ['txt', 'txt'], ['t xt'], ['a/b'], ['中文'], [[]]):
            with self.subTest(value=value), self.assertRaises(ValueError):
                package_app.content_extensions({'file_extensions': value})
        self.assertEqual(package_app.content_extensions({}), [])
        self.assertEqual(package_app.content_extensions({'file_extensions': []}), [])
        with self.assertRaises(ValueError):
            package_app.content_extensions({'kind': 'resources', 'file_extensions': ['txt']})
        extensions = ['a' * 300 + str(i) for i in range(600)]
        metadata = {'file_extensions': extensions}
        copy = package_app.content_extensions(metadata)
        self.assertEqual(copy, extensions)
        self.assertIsNot(copy, extensions)

    def test_installed_copy(self):
        with tempfile.TemporaryDirectory(prefix='content-package-') as directory:
            root = Path(directory)
            build_id = 'a' * 64
            profile = root / 'profile.json'
            profile.write_text(json.dumps({'build_id': build_id}))
            image = bytearray(256)
            image[:7] = b'\x7fELF\x01\x01\x01'
            struct.pack_into('<HHIIIIIHHHHHH', image, 16, 3, 40, 1, 0, 52, 0, 0, 52, 32, 1, 0, 0, 0)
            struct.pack_into('<8I', image, 52, 1, 0, 0, 0, 256, 256, 5, 4)
            marker = ('EPDAPP:' + build_id).encode() + b'\0'
            image[100:100 + len(marker)] = marker
            module = root / 'module.so'
            module.write_bytes(image)
            for app in ('books', 'gallery'):
                source = ROOT / 'modules' / app / 'app.json'
                package_app.build_package(source, module, profile, root / app)
                installed = json.loads((root / app / 'app.json').read_text(encoding='utf-8'))
                expected = json.loads(source.read_text(encoding='utf-8'))
                self.assertEqual(installed['file_extensions'], expected['file_extensions'])


if __name__ == '__main__':
    unittest.main()
