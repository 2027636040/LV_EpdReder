"""Release catalogue integrity, declared paths and metadata snapshots."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from release_catalog import catalogue, inspect_package


class ReleaseCatalogueTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='epd-release-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'app-profile.json').write_text(json.dumps(dict(board='board', build_id='a' * 64)))
        self.package = self.root / 'app-resources' / 'test'
        self.package.mkdir(parents=True)
        (self.package / 'test.so').write_bytes(b'module')
        index = hashlib.sha256(b'module').hexdigest() + '  test.so\n'
        (self.package / 'files.sha256').write_bytes(index.encode())
        self.metadata = dict(id='test', package_format=1, abi=1, build_id='a' * 64,
                             version=1, data_version=1, name='Test',
                             files_sha256=hashlib.sha256(index.encode()).hexdigest())
        self.save()

    def save(self):
        (self.package / 'app.json').write_text(json.dumps(self.metadata), encoding='utf-8')

    def save_index(self, data):
        (self.package / 'files.sha256').write_bytes(data)
        self.metadata['files_sha256'] = hashlib.sha256(data).hexdigest()
        self.save()

    def reindex(self):
        files = sorted(path for path in self.package.rglob('*') if path.is_file() and
                       path.name not in ('app.json', 'files.sha256'))
        self.save_index(''.join(hashlib.sha256(path.read_bytes()).hexdigest() + '  ' +
                                path.relative_to(self.package).as_posix() + '\n'
                                for path in files).encode('utf-8'))

    def test_valid_catalogue(self):
        result = catalogue(self.root)
        application = result['applications'][0]
        self.assertEqual(application['package'], 'app-resources/test')
        self.assertEqual(application['bytes'], sum(path.stat().st_size for path in self.package.iterdir()))
        self.assertEqual(application['manifest_sha256'],
                         hashlib.sha256((self.package / 'app.json').read_bytes()).hexdigest())
        self.assertEqual(result, catalogue(self.root, ['test', 'test']))

    def test_old_profile(self):
        self.metadata['build_id'] = 'b' * 64
        self.save()
        with self.assertRaisesRegex(ValueError, 'profile mismatch'):
            catalogue(self.root)

    def test_changed_payload(self):
        (self.package / 'test.so').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            catalogue(self.root)

    def test_extra_file(self):
        (self.package / 'old.so').write_bytes(b'old')
        with self.assertRaisesRegex(ValueError, 'unindexed'):
            catalogue(self.root)

    def test_missing_payload(self):
        (self.package / 'test.so').unlink()
        with self.assertRaisesRegex(ValueError, 'missing'):
            catalogue(self.root)

    def test_selection_escape(self):
        with self.assertRaises(ValueError):
            catalogue(self.root, ['../test'])

    def test_missing_declared_icon(self):
        self.metadata['icon'] = 'missing.ezip'
        self.save()
        with self.assertRaisesRegex(ValueError, 'missing declared payload'):
            catalogue(self.root)

    def test_missing_declared_library(self):
        self.metadata['libraries'] = {'missing': 256}
        self.save()
        with self.assertRaisesRegex(ValueError, 'missing declared payload'):
            catalogue(self.root)

    def test_valid_declared_payloads(self):
        self.metadata.update(icon='icon.ezip', libraries={'codec': 256})
        (self.package / 'icon.ezip').write_bytes(b'icon')
        (self.package / 'codecs').mkdir()
        (self.package / 'codecs/codec.so').write_bytes(b'private library')
        self.reindex()
        self.assertEqual(len(catalogue(self.root)['applications']), 1)

    def test_invalid_declared_paths(self):
        for field, value in (('icon', '../outside'), ('icon', None),
                             ('libraries', []), ('libraries', {'../outside': 256}),
                             ('libraries', {'nested/codec': 256}), ('libraries', {'test': 256})):
            with self.subTest(field=field, value=value):
                self.metadata[field] = value
                self.save()
                with self.assertRaises(ValueError):
                    catalogue(self.root)
                del self.metadata[field]

    def test_unsorted_index(self):
        (self.package / 'a.dat').write_bytes(b'resource')
        self.reindex()
        lines = (self.package / 'files.sha256').read_bytes().splitlines(keepends=True)
        self.save_index(b''.join(reversed(lines)))
        with self.assertRaisesRegex(ValueError, 'strictly increasing'):
            catalogue(self.root)

    def test_missing_final_lf(self):
        self.save_index((self.package / 'files.sha256').read_bytes().rstrip(b'\n'))
        with self.assertRaisesRegex(ValueError, 'LF-terminated'):
            catalogue(self.root)

    def test_crlf_index(self):
        self.save_index((self.package / 'files.sha256').read_bytes().replace(b'\n', b'\r\n'))
        with self.assertRaises(ValueError):
            catalogue(self.root)

    def test_duplicate_and_reserved_index_entries(self):
        original = (self.package / 'files.sha256').read_bytes()
        for data in (original + original,
                     hashlib.sha256(b'manifest').hexdigest().encode() + b'  app.json\n' + original):
            with self.subTest(index=data):
                self.save_index(data)
                with self.assertRaisesRegex(ValueError, 'duplicate/reserved'):
                    catalogue(self.root)

    def test_unicode_separator_is_part_of_filename(self):
        (self.package / 'a\u2028b.dat').write_bytes(b'resource')
        self.reindex()
        self.assertEqual(len(catalogue(self.root)['applications']), 1)

    def linked_firmware(self, junction=False, release_root=False):
        firmware = self.root / 'linked-firmware'
        firmware.mkdir()
        shutil.copyfile(self.root / 'app-profile.json', firmware / 'app-profile.json')
        link = firmware / 'app-resources'
        target = self.package.parent if release_root else self.package
        if not release_root:
            link.mkdir()
            link /= 'test'
        if junction:
            result = subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(link), str(target)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.addCleanup(link.rmdir)
        else:
            try:
                link.symlink_to(target, target_is_directory=True)
            except OSError as error:
                if os.name == 'nt' and error.winerror == 1314:
                    self.skipTest('Windows symlink privilege is unavailable')
                raise
            self.addCleanup(link.unlink)
        return firmware

    def test_package_root_symlink(self):
        firmware = self.linked_firmware()
        for names in (None, ['test']):
            with self.subTest(names=names), self.assertRaisesRegex(ValueError, 'Linked package root'):
                catalogue(firmware, names)

    @unittest.skipUnless(os.name == 'nt', 'Windows directory junction')
    def test_package_root_junction(self):
        firmware = self.linked_firmware(junction=True)
        for names in (None, ['test']):
            with self.subTest(names=names), self.assertRaisesRegex(ValueError, 'Linked package root'):
                catalogue(firmware, names)

    def test_release_root_symlink(self):
        firmware = self.linked_firmware(release_root=True)
        with self.assertRaisesRegex(ValueError, 'outside release root'):
            catalogue(firmware)

    @unittest.skipUnless(os.name == 'nt', 'Windows directory junction')
    def test_release_root_junction(self):
        firmware = self.linked_firmware(junction=True, release_root=True)
        with self.assertRaisesRegex(ValueError, 'outside release root'):
            catalogue(firmware)

    def test_package_outside_release_root(self):
        with self.assertRaisesRegex(ValueError, 'outside release root'):
            inspect_package(self.package, 'a' * 64, self.root / 'other-release-root')

    def assert_snapshot(self, path, replacement):
        original = path.read_bytes()
        read_bytes = Path.read_bytes
        reads = []

        def replace_after_read(current):
            data = read_bytes(current)
            if current == path:
                reads.append(current)
                current.write_bytes(replacement)
            return data

        with patch.object(Path, 'read_bytes', replace_after_read):
            result = catalogue(self.root)
        self.assertEqual(reads, [path])
        return result, hashlib.sha256(original).hexdigest(), len(original)

    def test_manifest_parsing_and_digest_use_same_bytes(self):
        replacement = json.dumps(dict(self.metadata, version=99, name='Changed generation')).encode()
        result, digest, size = self.assert_snapshot(self.package / 'app.json', replacement)
        application = result['applications'][0]
        self.assertEqual(application['version'], 1)
        self.assertEqual(application['name'], 'Test')
        self.assertEqual(application['manifest_sha256'], digest)
        self.assertEqual(application['bytes'], size + (self.package / 'files.sha256').stat().st_size + len(b'module'))

    def test_profile_parsing_and_digest_use_same_bytes(self):
        replacement = json.dumps(dict(board='changed-board', build_id='b' * 64)).encode()
        result, digest, _ = self.assert_snapshot(self.root / 'app-profile.json', replacement)
        self.assertEqual(result['board'], 'board')
        self.assertEqual(result['build_id'], 'a' * 64)
        self.assertEqual(result['profile_sha256'], digest)

    def test_index_parsing_and_digest_use_same_bytes(self):
        result, digest, size = self.assert_snapshot(self.package / 'files.sha256', b'changed index\n')
        application = result['applications'][0]
        self.assertEqual(application['files_sha256'], digest)
        self.assertEqual(application['bytes'], (self.package / 'app.json').stat().st_size + size + len(b'module'))


if __name__ == '__main__':
    unittest.main()
