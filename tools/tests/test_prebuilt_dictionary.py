"""Repository dictionary round trips, verification and failed-build cleanup."""
import gzip
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import prebuilt_dictionary


class PrebuiltDictionaryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'library.wdb'
        self.report = self.root / 'library.wdb.json'
        self.archive = self.root / 'repository/library.wdb.gz'
        self.restored = self.root / 'build/library.wdb'
        self.content = ('EPDWD02\0hello\0你好\0' * 100000).encode('utf-8')
        self.source.write_bytes(self.content)
        self.metadata = {'bytes': len(self.content), 'sha256': hashlib.sha256(self.content).hexdigest()}
        self.write_report()

    def write_report(self):
        self.report.write_text(json.dumps(self.metadata), encoding='utf-8')

    def test_deterministic_round_trip(self):
        prebuilt_dictionary.transfer(self.source, self.report, self.archive, compress=True)
        second = self.root / 'other-name.gz'
        prebuilt_dictionary.transfer(self.source, self.report, second, compress=True)
        self.assertEqual(self.archive.read_bytes(), second.read_bytes())
        self.assertEqual(prebuilt_dictionary.unpack(self.archive, self.report, self.restored), self.restored)
        self.assertEqual(self.restored.read_bytes(), self.content)
        self.assertEqual(self.archive.read_bytes()[4:8], b'\0' * 4)
        self.assertFalse(list(self.root.rglob('*.tmp')))

    def test_size_and_hash_mismatch_preserve_previous_output(self):
        self.archive.parent.mkdir()
        self.archive.write_bytes(gzip.compress(self.content, mtime=0))
        self.restored.parent.mkdir()
        self.restored.write_bytes(b'previous dictionary')
        for changes in ({'bytes': len(self.content) - 1}, {'bytes': len(self.content) + 1},
                        {'bytes': len(self.content), 'sha256': '0' * 64}):
            with self.subTest(changes=changes):
                self.metadata.update(changes)
                self.write_report()
                with self.assertRaises(ValueError):
                    prebuilt_dictionary.unpack(self.archive, self.report, self.restored)
                self.assertEqual(self.restored.read_bytes(), b'previous dictionary')
                self.assertFalse(list(self.root.rglob('*.tmp')))

    def test_truncated_archive_preserves_previous_output(self):
        self.archive.parent.mkdir()
        self.archive.write_bytes(gzip.compress(self.content, mtime=0)[:-8])
        self.restored.parent.mkdir()
        self.restored.write_bytes(b'previous dictionary')
        with self.assertRaises(EOFError):
            prebuilt_dictionary.unpack(self.archive, self.report, self.restored)
        self.assertEqual(self.restored.read_bytes(), b'previous dictionary')
        self.assertFalse(list(self.root.rglob('*.tmp')))

    def test_publication_failure_preserves_previous_output(self):
        self.archive.parent.mkdir()
        self.archive.write_bytes(gzip.compress(self.content, mtime=0))
        self.restored.parent.mkdir()
        self.restored.write_bytes(b'previous dictionary')
        with patch.object(prebuilt_dictionary.os, 'replace', side_effect=OSError('disk failure')):
            with self.assertRaises(OSError):
                prebuilt_dictionary.unpack(self.archive, self.report, self.restored)
        self.assertEqual(self.restored.read_bytes(), b'previous dictionary')
        self.assertFalse(list(self.root.rglob('*.tmp')))

    def test_pack_rejects_mismatched_report(self):
        self.metadata['sha256'] = '0' * 64
        self.write_report()
        with self.assertRaises(ValueError):
            prebuilt_dictionary.transfer(self.source, self.report, self.archive, compress=True)
        self.assertFalse(self.archive.exists())
        self.assertFalse(list(self.root.rglob('*.tmp')))

    def test_invalid_metadata_and_input_overwrite(self):
        for changes in ({'bytes': -1}, {'bytes': True}, {'sha256': 'invalid'}):
            with self.subTest(changes=changes):
                self.metadata = {'bytes': len(self.content), 'sha256': hashlib.sha256(self.content).hexdigest()}
                self.metadata.update(changes)
                self.write_report()
                with self.assertRaises(ValueError):
                    prebuilt_dictionary.transfer(self.source, self.report, self.archive, compress=True)
        self.metadata = {'bytes': len(self.content), 'sha256': hashlib.sha256(self.content).hexdigest()}
        self.write_report()
        for destination in (self.source, self.report):
            with self.assertRaisesRegex(ValueError, 'differ from its inputs'):
                prebuilt_dictionary.transfer(self.source, self.report, destination, compress=True)
        self.assertEqual(self.source.read_bytes(), self.content)


if __name__ == '__main__':
    unittest.main()
