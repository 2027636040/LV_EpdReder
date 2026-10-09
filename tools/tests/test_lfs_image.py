"""Exercise LittleFS image generation with the SDK's Windows executable."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import lfs_image

SDK_TOOL = Path(__file__).resolve().parents[2] / 'SiFli-SDK/tools/mklfsimg/mklfsimg.exe'


@unittest.skipUnless(os.name == 'nt' and SDK_TOOL.is_file(), 'Requires the Windows SDK tool')
class NativeLfsImageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='epd-lfs-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.disk = self.root / 'disk'
        self.disk.mkdir()
        self.target = self.root / 'fs_root.bin'
        self.ptab = self.root / 'ptab.json'
        self.ptab.write_text(json.dumps([{'base': '0x12000000', 'regions': [
            {'name': 'fs_root', 'offset': '0x0', 'max_size': '0x20000'}]}]), encoding='utf-8')

    def test_empty_image_without_visual_studio_environment(self):
        with patch.dict(os.environ, {'ProgramFiles(x86)': str(self.root / 'missing-program-files')}):
            lfs_image.build_image(self.disk, self.target, SDK_TOOL, self.ptab)
        self.assertEqual(self.target.stat().st_size, 0x20000)
        self.assertTrue(b'littlefs' in self.target.read_bytes()[:8192])

    def test_ascii_paths_preserve_utf8_file_contents(self):
        directory = self.disk / 'apps/sample/res'
        directory.mkdir(parents=True)
        payload = '中文内容和 English text'.encode('utf-8')
        (directory / 'sample.txt').write_bytes(payload)
        lfs_image.build_image(self.disk, self.target, SDK_TOOL, self.ptab)
        image = self.target.read_bytes()
        self.assertTrue(b'sample.txt' in image)
        self.assertTrue(payload in image)

    def test_native_filename_encoding(self):
        name = '中文测试.txt'
        (self.disk / name).write_bytes(b'filename encoding probe')
        subprocess.run([str(SDK_TOOL), '-c', str(self.disk), '-b', '4096', '-r', '32',
                        '-p', '256', '-s', str(0x20000), '-i', str(self.target)], check=True)
        image = self.target.read_bytes()
        self.assertTrue(name.encode('mbcs', errors='replace') in image)

    def test_reject_incompatible_filename_encoding(self):
        name = '中文测试.txt'
        (self.disk / name).write_bytes(b'filename encoding probe')
        if name.encode('mbcs', errors='replace') != name.encode('utf-8'):
            with self.assertRaisesRegex(ValueError, 'Windows ANSI filenames'):
                lfs_image.build_image(self.disk, self.target, SDK_TOOL, self.ptab)
            self.assertFalse(self.target.exists())
        else:
            lfs_image.build_image(self.disk, self.target, SDK_TOOL, self.ptab)
            self.assertTrue(name.encode('utf-8') in self.target.read_bytes())


if __name__ == '__main__':
    unittest.main()
