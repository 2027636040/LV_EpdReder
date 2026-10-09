"""Package counts, long manifests and retained input validation."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('package_app', Path(__file__).resolve().parents[1] / 'package_app.py')
package_app = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package_app)


class PackageCapacity(unittest.TestCase):
    def test_growth_and_validation(self):
        with tempfile.TemporaryDirectory(prefix='epd-package-test-') as directory:
            root = Path(directory)
            build_id = 'a' * 64
            profile = root / 'profile.json'
            profile.write_text(json.dumps({'build_id': build_id}))
            manifest = root / 'source.json'
            manifest.write_text(json.dumps(dict(abi=1, id='sample', version=1,
                                                name='Sample', data_version=1)))
            image = bytearray(256)
            image[:7] = b'\x7fELF\x01\x01\x01'
            struct.pack_into('<HHIIIIIHHHHHH', image, 16, 3, 40, 1, 0, 52, 0, 0, 52, 32, 1, 0, 0, 0)
            struct.pack_into('<8I', image, 52, 1, 0, 0, 0, 256, 256, 5, 4)
            marker = ('EPDAPP:' + build_id).encode() + b'\0'
            image[100:100 + len(marker)] = marker
            module = root / 'sample.so'
            module.write_bytes(image)
            libraries = []
            for number in range(40):
                library = root / f'lib{number:02}.so'
                library.write_bytes(image)
                libraries.append(library)
            resources = root / 'resources'
            resources.mkdir()
            for number in range(600):
                (resources / f'{number:03}.dat').write_text(str(number))
            output = root / 'package'
            result = package_app.build_package(manifest, module, profile, output,
                                               resources=resources, libraries=libraries)
            self.assertEqual(result['files'], 641)
            self.assertEqual(len(result['libraries']), 40)
            self.assertGreater((output / 'app.json').stat().st_size, 1024)
            index = (output / 'files.sha256').read_bytes()
            metadata = json.loads((output / 'app.json').read_text())
            self.assertEqual(metadata['files_sha256'], hashlib.sha256(index).hexdigest())
            self.assertEqual(len(index.splitlines()), 641)
            for line in index.decode().splitlines():
                digest, name = line.split('  ', 1)
                self.assertEqual(digest, hashlib.sha256((output / name).read_bytes()).hexdigest())
            with self.assertRaises(ValueError):
                package_app.build_package(manifest, module, profile, root / 'duplicate',
                                          libraries=[libraries[0], libraries[0]])
            with self.assertRaises(ValueError):
                package_app.build_package(manifest, module, profile, root / 'collision', libraries=[module])
            for path in ('../outside', 'res/../outside', '/absolute', 'a\\b', 'a:b', 'x' * 160):
                with self.assertRaises(ValueError):
                    package_app.relative_name(path)


if __name__ == '__main__':
    unittest.main()
