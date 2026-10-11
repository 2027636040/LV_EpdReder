"""Tests for symbol identity, package association and explicit source export."""
import hashlib
import ast
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch, mock_open
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import debug_artifacts as debug


class DebugArtifactTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='epd-symbols-')
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        self.build = self.repo / 'project/build_test'
        self.build.mkdir(parents=True)
        self.profile = {'board': 'test', 'compiler': 'gcc fixture', 'module_cflags': '-g',
                        'module_ldflags': '-shared', 'build_id': 'abi'}
        (self.build / 'app-profile.json').write_text(json.dumps(self.profile))
        for name in ('rtconfig.h', 'cconfig.h', 'epd_app_profile.h', 'rtua.py', 'link_copy.lds'):
            (self.build / name).write_text(name)
        (self.build / 'main.elf').write_bytes(b'firmware A')
        self.state = {'project': {'root': str(self.repo), 'commit': 'a' * 40, 'dirty': True,
                                  'diff_sha256': hashlib.sha256(b'patch').hexdigest(), 'untracked': {}}}
        mock_state = patch.object(debug, 'source_state', return_value=self.state)
        mock_state.start()
        self.addCleanup(mock_state.stop)
        # Integration builds test real GNU notes; here fixture bytes identify the pair.
        identities = patch.object(debug, 'build_id', side_effect=lambda path, tool: hashlib.sha1(
            Path(path).read_bytes().split(b'\nDEBUG')[0]).hexdigest())
        identities.start()
        self.addCleanup(identities.stop)

    def app(self, name='books', private=True, content=b'module A'):
        package = self.build / 'app-resources' / name
        package.mkdir(parents=True, exist_ok=True)
        (package / 'app.json').write_text(json.dumps({'version': 1}))
        images = [(package / (name + '.so'), self.repo / 'modules' / name / 'output' / (name + '.so.nostrip'), content)]
        if private:
            images.append((package / 'codecs/bk_mobi.so',
                           self.repo / 'modules' / name / 'codecs/mobi/output/bk_mobi.so.nostrip', b'private'))
        for install, symbols, data in images:
            install.parent.mkdir(parents=True, exist_ok=True)
            symbols.parent.mkdir(parents=True, exist_ok=True)
            install.write_bytes(data)
            symbols.write_bytes(data + b'\nDEBUG symbols')
        return images

    def test_platform_does_not_inspect_unselected_apps(self):
        result = debug.archive(self.repo, self.build, [], 'readelf')
        self.assertEqual(len(result['symbols']), 1)
        self.assertEqual(result['builds'][result['current_firmware']]['applications'], {})

    def test_private_symbols_and_history_are_not_installed(self):
        self.app()
        result = debug.archive(self.repo, self.build, ['books'], 'readelf')
        firmware = result['builds'][result['current_firmware']]
        self.assertEqual(len(firmware['applications']['books']['modules']), 2)
        self.assertEqual(len(result['symbols']), 3)
        self.assertFalse(list((self.build / 'app-resources').rglob('*.nostrip')))
        previous = set(result['symbols'])
        self.app(content=b'module B')
        result = debug.archive(self.repo, self.build, ['books'], 'readelf')
        self.assertTrue(previous < set(result['symbols']))
        self.assertEqual(len(result['symbols']), 4)

    def test_missing_symbols_fail_instead_of_using_same_name(self):
        images = self.app(private=False)
        images[0][1].unlink()
        with self.assertRaises(FileNotFoundError):
            debug.archive(self.repo, self.build, ['books'], 'readelf')

    def test_mismatched_install_rejected(self):
        images = self.app(private=False)
        images[0][0].write_bytes(b'different build')
        with self.assertRaisesRegex(ValueError, 'mismatch'):
            debug.archive(self.repo, self.build, ['books'], 'readelf')
        self.assertFalse((self.build / 'debug-artifacts/manifest.json').exists())

    def test_missing_archive_repaired(self):
        self.app()
        result = debug.archive(self.repo, self.build, ['books'], 'readelf')
        path = self.build / 'debug-artifacts' / next(iter(result['symbols'].values()))['file']
        path.unlink()
        debug.archive(self.repo, self.build, ['books'], 'readelf')
        self.assertTrue(path.is_file())

    def test_corrupt_existing_archive_not_overwritten(self):
        result = debug.archive(self.repo, self.build, [], 'readelf')
        path = self.build / 'debug-artifacts' / next(iter(result['symbols'].values()))['file']
        path.write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            debug.archive(self.repo, self.build, [], 'readelf')
        self.assertEqual(path.read_bytes(), b'corrupt')

    def test_explicit_export_contains_symbols_diff_and_untracked_input(self):
        source = self.repo / 'new.c'
        source.write_text('new source')
        self.state['project']['untracked'] = {'new.c': debug.sha256(source)}
        self.app()
        debug.archive(self.repo, self.build, ['books'], 'readelf')
        output = self.repo / 'debug.zip'
        with patch.object(debug, 'run', return_value=b'patch'):
            debug.export_bundle(self.repo, self.build, output)
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.read('sources/project/untracked/new.c'), b'new source')
            self.assertEqual(archive.read('sources/project/changes.patch'), b'patch')
            self.assertEqual(len([name for name in archive.namelist() if name.endswith('.nostrip')]), 2)
            self.assertFalse(any('app-resources' in name for name in archive.namelist()))

    def test_source_drift_does_not_mislabel_debug_bundle(self):
        debug.archive(self.repo, self.build, [], 'readelf')
        self.state['project']['commit'] = 'b' * 40
        with self.assertRaisesRegex(ValueError, 'Sources changed'):
            debug.export_bundle(self.repo, self.build, self.repo / 'debug.zip')

    def test_only_platform_rebuild_refreshes_unchanged_firmware_provenance(self):
        debug.archive(self.repo, self.build, [], 'readelf')
        self.state['project']['commit'] = 'b' * 40
        debug.archive(self.repo, self.build, [], 'readelf')
        with self.assertRaisesRegex(ValueError, 'Sources changed'):
            debug.export_bundle(self.repo, self.build, self.repo / 'debug.zip')
        debug.archive(self.repo, self.build, [], 'readelf', firmware_built=True)
        with patch.object(debug, 'run', return_value=b'patch'):
            debug.export_bundle(self.repo, self.build, self.repo / 'debug.zip')


class NoteTests(unittest.TestCase):
    def test_real_note_output_and_missing_identity(self):
        with patch.object(debug, 'run', return_value=b'Build ID: ' + b'1' * 40 + b'\n'):
            self.assertEqual(debug.build_id('fixture', 'readelf'), '1' * 40)
        for text in (b'', b'Build ID: abcd\n', b'Build ID: ' + b'1' * 40 + b'\nBuild ID: ' + b'2' * 40):
            with patch.object(debug, 'run', return_value=text), self.assertRaises(ValueError):
                debug.build_id('fixture', 'readelf')


class BuildDefinitionTests(unittest.TestCase):
    def test_generated_module_definitions_have_stable_order(self):
        source = Path(__file__).resolve().parents[2] / 'SiFli-SDK/tools/build/ua.py'
        tree = ast.parse(source.read_text(encoding='utf-8'))
        function = next(node for node in tree.body
                        if isinstance(node, ast.FunctionDef) and node.name == 'PrepareUA')
        scope = {'os': os, 'print': lambda *args: None}
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), 'exec'), scope)
        with patch('builtins.open', mock_open()) as output:
            scope['PrepareUA']([{'CPPDEFINES': ['Z', 'A', 'Z', ('VALUE', 3)]}], 'sdk', 'project')
        text = ''.join(call.args[0] for call in output().write.call_args_list)
        self.assertIn("CPPDEFINES=[('VALUE', 3), 'A', 'Z']", text)


if __name__ == '__main__':
    unittest.main()
