"""Application selection and one-way firmware/package build dependencies."""
from pathlib import Path
import gzip
import hashlib
import json
import sys
import tempfile
import types
import unittest
from unittest.mock import patch, Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import native_apps
import lfs_image


class BuildEnv(dict):
    def __init__(self, root):
        super().__init__(build_dir=str(root / 'build'), APP_NM='nm',
                         PARTITION_TABLE=str(root / 'ptab.json'))
        self.commands = []
        self.precious = []

    def Command(self, targets, sources, action):
        targets = [targets] if isinstance(targets, str) else targets
        self.commands.append((targets, sources, action))
        return targets

    def Precious(self, nodes):
        self.precious.extend(nodes)


class AppBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='epd-app-build-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = BuildEnv(self.root)
        self.script = types.ModuleType('SCons.Script')
        self.script.Action = lambda action, description: action
        self.script.Copy = lambda target, source: ('copy', target, source)
        self.script.Value = lambda value: ('value', str(value))
        self.modules = patch.dict(sys.modules, {'SCons': types.ModuleType('SCons'),
                                               'SCons.Script': self.script})
        self.modules.start()
        self.addCleanup(self.modules.stop)

    def test_selection(self):
        for selection in ('', 'none', ' , '):
            self.assertEqual(native_apps.select_apps(selection), [])
        self.assertEqual(native_apps.select_apps('books, weather,books'), ['books', 'weather'])
        self.assertEqual(native_apps.select_apps('all'), list(native_apps.PACKAGE_BUILDERS))
        for selection in ('unknown', 'all,books', 'none,books', '../words'):
            with self.assertRaises(ValueError):
                native_apps.select_apps(selection)

    def test_platform_needs_no_modules_config_or_dictionary(self):
        with patch.object(Path, 'read_text', side_effect=AssertionError('unexpected file read')):
            self.assertEqual(native_apps.add_selected_packages(self.env, self.root, self.root, []), {})
        self.assertEqual(self.env.commands, [])

    def test_only_selected_sources_are_required(self):
        module = self.root / 'modules/weather'
        module.mkdir(parents=True)
        for name in ('SConstruct', 'app.json'):
            (module / name).touch()
        build = Path(self.env['build_dir'])
        build.mkdir()
        (build / 'rtconfig.h').write_text('#define LV_COLOR_DEPTH 16\n#define EZIP_PAL_SUPPORT\n'
                                        '#define EZIP_PAL_SUPPORT_1\n', encoding='utf-8')
        weather, words = Mock(return_value='weather package'), Mock()
        with patch.dict(native_apps.PACKAGE_BUILDERS, weather=weather, words=words):
            packages = native_apps.add_selected_packages(self.env, self.root, self.root, ['weather'])
            self.assertEqual(packages, {'weather': 'weather package'})
            weather.assert_called_once_with(self.env, self.root, self.root, True, 1)
            words.assert_not_called()
        with self.assertRaisesRegex(ValueError, 'Selected application source is missing'):
            native_apps.add_selected_packages(self.env, self.root, self.root, ['books'])

    def test_package_depends_on_firmware_but_module_does_not(self):
        root, nodes = native_apps.add_package(self.env, self.root, self.root, 'weather', {})
        module, package = self.env.commands
        firmware = str(Path(self.env['build_dir']) / 'main.elf')
        self.assertNotIn(firmware, module[1])
        self.assertIn(firmware, package[1])
        self.assertIn(str(Path(self.env['build_dir']) / 'rtua.py'), module[1])
        self.assertNotIn(str(self.root / 'project/rtua.py'), module[1])
        self.assertEqual(root, Path(self.env['build_dir']) / 'app-resources/weather')
        self.assertEqual(nodes, package[0])
        self.assertEqual(self.env.precious, nodes)
        with patch.object(native_apps, 'verify', side_effect=ValueError('Unexported imports')), \
                patch.object(native_apps, 'build_package') as pack:
            with self.assertRaisesRegex(ValueError, 'Unexported imports'):
                package[2](None, None, self.env)
            pack.assert_not_called()

    def test_words_requires_both_dictionary_files(self):
        with self.assertRaisesRegex(ValueError, 'words requires dictionary and report'):
            native_apps.add_words_package(self.env, self.root, self.root)
        dictionary = self.root / 'cet4.wdb'
        dictionary.touch()
        with self.assertRaisesRegex(ValueError, 'cet4.wdb.json'):
            native_apps.add_words_package(self.env, self.root, self.root, dictionary=dictionary)

    def test_words_packages_prebuilt_unified_dictionary_and_provenance(self):
        module = self.root / 'modules/words'
        archive = module / 'assets/dictionary/library.wdb.gz'
        archive.parent.mkdir(parents=True)
        content = b'pre-generated dictionary fixture'
        archive.write_bytes(gzip.compress(content, mtime=0))
        report = archive.with_suffix('.json')
        report.write_text(json.dumps({'bytes': len(content), 'sha256': hashlib.sha256(content).hexdigest()}))
        dictionary = Path(self.env['build_dir']) / 'words-dictionary/library.wdb'
        with patch.object(native_apps, 'add_package', return_value='words package') as package:
            self.assertEqual(native_apps.add_words_package(self.env, self.root, self.root), 'words package')
            resources = package.call_args.args[-1]
            self.assertEqual(resources['library.wdb'], dictionary)
            self.assertEqual(resources['library.wdb.json'], report)
            self.assertEqual(resources['SOURCES.txt'], module / 'SOURCES.txt')
            self.assertIn('LICENSE.ecdict', resources)
            self.assertIn('LICENSE.fsrs', resources)
        # Restore the pre-generated WDB; no source CSV/JSON conversion or download action.
        self.assertEqual(len(self.env.commands), 2)
        targets, sources, action = self.env.commands[1]
        self.assertEqual(targets, [str(dictionary)])
        self.assertEqual(sources[:2], [str(archive), str(report)])
        self.assertTrue(sources[2].endswith('prebuilt_dictionary.py'))
        self.assertFalse(dictionary.exists())
        self.assertEqual(action(targets, sources, self.env), 0)
        self.assertEqual(dictionary.read_bytes(), content)

    def test_words_custom_dictionary_does_not_need_repository_archive(self):
        dictionary = self.root / 'custom.wdb'
        dictionary.touch()
        report = dictionary.with_suffix('.wdb.json')
        report.touch()
        with patch.object(native_apps, 'add_package', return_value='custom package') as package:
            self.assertEqual(native_apps.add_words_package(self.env, self.root, self.root,
                                                          dictionary=dictionary), 'custom package')
            resources = package.call_args.args[-1]
            self.assertEqual(resources['library.wdb'], dictionary)
            self.assertEqual(resources['library.wdb.json'], report)
        self.assertEqual(len(self.env.commands), 1)

    def test_filesystem_depends_only_on_selected_packages(self):
        contents = self.root / 'disk'
        contents.mkdir()
        selected = Path(self.env['build_dir']) / 'app-resources/weather'
        for packages in ([], [(selected, ['weather-package'])]):
            with patch.object(lfs_image, 'verify_import', return_value=contents):
                lfs_image.add_image(self.env, self.root, migration=str(self.root), packages=packages)
            sources = self.env.commands[-1][1]
            self.assertEqual('weather-package' in sources, bool(packages))
            self.assertIn(('value', str([p.name for p, _ in packages])), sources)
            self.assertIn(str(self.root / 'tools/mklfsimg/mklfsimg.exe'), sources)
        # One image action per call, with no host-tool compilation action.
        self.assertEqual(len(self.env.commands), 2)


if __name__ == '__main__':
    unittest.main()
