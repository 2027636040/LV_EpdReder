"""SDK integrity tests and optional real, relocated cross-compilation tests.

Set EPD_APP_SDK_BUNDLES to os.pathsep-separated exported SDK directories to run
the compiler integration test. No firmware build or device is needed.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import app_sdk


def make_profile(root, board='test-board'):
    root.mkdir(parents=True, exist_ok=True)
    for name in ('rtconfig.h', 'cconfig.h'):
        (root / name).write_text('/* fixture */\n', encoding='utf-8')
    profile = {'profile_format': 1, 'board': board, 'compiler': 'test compiler',
               'module_cflags': '-fPIC', 'module_ldflags': '-shared -nostdlib',
               'configuration': {name: app_sdk.digest(root / name)
                                 for name in ('rtconfig.h', 'cconfig.h')},
               'platform_interface': {}}
    profile['build_id'] = hashlib.sha256(json.dumps(profile, sort_keys=True, ensure_ascii=True).encode()).hexdigest()
    app_sdk.write_json(root / 'app-profile.json', profile)
    (root / 'epd_app_profile.h').write_text('#define EPD_APP_BUILD_ID "' + profile['build_id'] + '"\n')
    return profile


def make_sdk(root):
    profile = make_profile(root / 'config')
    (root / 'firmware-exports.txt').write_text('epd_app_label\nrt_snprintf\n')
    ua = {'CPPPATH': ['config'], 'CPPDEFINES': app_sdk.MODULE_DEFINES}
    app_sdk.write_json(root / 'rtua.json', ua)
    cflags, ldflags = app_sdk.compiler_flags(profile)
    recipe = {'sdk_format': app_sdk.FORMAT, 'board': profile['board'], 'build_id': profile['build_id'],
              'compiler': profile['compiler'], 'cflags': cflags,
              'ldflags': ldflags + ['-e', '0', '-Wl,--build-id=sha1'],
              'strip_flags': ['-R', '.hash', '--keep-section=.note.gnu.build-id'],
              'include_paths': ua['CPPPATH'], 'defines': ua['CPPDEFINES'],
              'export_count': 2, 'public_headers': [],
              'files': {p.relative_to(root).as_posix(): app_sdk.digest(p)
                        for p in root.rglob('*') if p.is_file()}}
    app_sdk.write_json(root / 'sdk.json', recipe)
    return recipe


def make_module(path, identity):
    data = bytearray(256)
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', data, 16, 3, 40, 1, 0, 52, 0, 0, 52, 32, 1, 0, 0, 0)
    struct.pack_into('<8I', data, 52, 1, 0, 0, 0, 256, 300, 5, 4)
    marker = ('EPDAPP:' + identity).encode() + b'\0'
    data[100:100 + len(marker)] = marker
    path.write_bytes(data)


class AppSDKTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='app sdk unit ')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.recipe = make_sdk(self.root)

    def export_fixture(self):
        repo = self.root / 'repo'
        firmware = repo / 'project/build_test'
        profile = make_profile(firmware)
        public = repo / 'src/platform/epd_app.h'
        public.parent.mkdir(parents=True)
        public.write_text('#include "app_content.h"\n')
        content = public.with_name('app_content.h')
        content.write_text('#include "leaf.h"\n')
        leaf = public.with_name('leaf.h')
        leaf.write_text('/* Recursive API dependency. */\n')
        profile['platform_interface'] = {public.name: app_sdk.digest(public)}
        del profile['build_id']
        profile['build_id'] = hashlib.sha256(json.dumps(profile, sort_keys=True, ensure_ascii=True).encode()).hexdigest()
        app_sdk.write_json(firmware / 'app-profile.json', profile)
        (firmware / 'epd_app_profile.h').write_text('#define EPD_APP_BUILD_ID "' + profile['build_id'] + '"\n')
        (firmware / 'main.elf').write_bytes(profile['build_id'].encode() + b'\0')
        (firmware / 'rtua.py').write_text('# fixture, read_ua is mocked\n')
        (repo / 'docs').mkdir()
        (repo / 'docs/APP_SDK.md').write_text('SDK fixture\n')

        def preprocess(arguments, directory, toolchain):
            depfile = Path(arguments[arguments.index('-MF') + 1])
            paths = [Path(directory) / 'public.c', public, content, leaf]
            depfile.write_text('sdk: ' + ' '.join(str(p).replace(' ', '\\ ') for p in paths))

        return repo, firmware, public, preprocess

    def test_export_collects_new_public_header_and_recursive_dependency(self):
        repo, firmware, public, preprocess = self.export_fixture()
        destination = self.root / 'exported'
        with patch.object(app_sdk, 'check_compiler'), \
                patch.object(app_sdk, 'firmware_exports', return_value=['epd_app_label']), \
                patch.object(app_sdk, 'read_ua', return_value=([str(public.parent)], [])), \
                patch.object(app_sdk, 'gcc', side_effect=preprocess):
            app_sdk.export_sdk(repo, firmware, destination)
        recipe = app_sdk.load_sdk(destination)
        for name in ('epd_app.h', 'app_content.h', 'leaf.h'):
            self.assertIn('include/src/platform/' + name, recipe['files'])
        self.assertFalse((destination / 'main.elf').exists())
        self.assertFalse((destination / 'src').exists())

    def test_export_refuses_stale_public_headers(self):
        repo, firmware, public, preprocess = self.export_fixture()
        public.write_text('changed API\n')
        destination = self.root / 'exported'
        with patch.object(app_sdk, 'check_compiler'), \
                patch.object(app_sdk, 'firmware_exports', return_value=['epd_app_label']), \
                patch.object(app_sdk, 'read_ua', return_value=([str(public.parent)], [])), \
                patch.object(app_sdk, 'gcc') as compiler:
            with self.assertRaisesRegex(ValueError, 'Public header differs'):
                app_sdk.export_sdk(repo, firmware, destination)
            compiler.assert_not_called()
        self.assertFalse(destination.exists())

    def test_relocation_and_corrupt_recursive_header(self):
        header = self.root / 'include/nested/leaf.h'
        header.parent.mkdir(parents=True)
        header.write_text('/* leaf */')
        self.recipe['files']['include/nested/leaf.h'] = app_sdk.digest(header)
        app_sdk.write_json(self.root / 'sdk.json', self.recipe)
        moved = self.root / 'moved sdk'
        shutil.copytree(self.root, moved, ignore=shutil.ignore_patterns('moved sdk'))
        self.assertEqual(app_sdk.load_sdk(moved)['build_id'], self.recipe['build_id'])
        (moved / 'include/nested/leaf.h').write_text('/* damaged */')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            app_sdk.load_sdk(moved)

    def test_profile_configuration_and_header_fingerprints(self):
        for name, replacement, message in (
                ('rtconfig.h', 'changed', 'configuration fingerprint'),
                ('epd_app_profile.h', '#define EPD_APP_BUILD_ID "wrong"', 'fingerprint mismatch'),
                ('app-profile.json', '{"profile_format": 1, "build_id": "wrong"}', 'profile fingerprint')):
            path = self.root / 'config' / name
            previous = path.read_bytes()
            with self.subTest(name=name):
                path.write_text(replacement)
                with self.assertRaisesRegex(ValueError, message):
                    app_sdk.read_profile(self.root / 'config')
                path.write_bytes(previous)

    def test_fixed_recipe_cannot_change_abi_flags(self):
        self.recipe['cflags'].append('-fshort-enums')
        app_sdk.write_json(self.root / 'sdk.json', self.recipe)
        with self.assertRaisesRegex(ValueError, 'recipe/profile mismatch'):
            app_sdk.load_sdk(self.root)

    def test_reject_external_manifest_paths(self):
        for name in ('../secret.h', '/secret.h', 'D:/secret.h', 'include\\secret.h', 'a/./b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                app_sdk.sdk_path(self.root, name)

    def test_rtua_is_read_without_executing_python(self):
        path = self.root / 'rtua.py'
        path.write_text('raise RuntimeError("must not execute")\n'
                        'def GetCPPPATH(BSP_ROOT, RTT_ROOT):\n'
                        '    CPPPATH = [BSP_ROOT + "/build", RTT_ROOT + "/include"]\n'
                        '    return CPPPATH\n'
                        'def GetCPPDEFINES():\n'
                        '    CPPDEFINES = ["BUILD_DLMODULE", ("VALUE", "42")]\n'
                        '    return CPPDEFINES\n')
        paths, defines = app_sdk.read_ua(path, 'project', 'rtt')
        self.assertEqual(paths, ['project/build', 'rtt/include'])
        self.assertEqual(app_sdk.definitions(defines), ['-DBUILD_DLMODULE', '-DVALUE=42'])
        path.write_text(path.read_text().replace('["BUILD_DLMODULE", ("VALUE", "42")]', 'unknown()'))
        with self.assertRaisesRegex(ValueError, 'Unsupported generated rtua'):
            app_sdk.read_ua(path, '', '')

    def test_dependency_paths_with_spaces_hashes_and_drive(self):
        text = 'sdk: C:/app\\ sdk/header.h \\\n D:/folder/leaf.h path\\#name.h dollar$$.h\n'
        self.assertEqual(app_sdk.dependencies(text), [Path(p).resolve() for p in
                         ('C:/app sdk/header.h', 'D:/folder/leaf.h', 'path#name.h', 'dollar$.h')])
        if os.name == 'nt':
            self.assertEqual(app_sdk.dependencies(r'sdk: C:\Users\user\file.h'),
                             [Path('C:/Users/user/file.h')])

    def test_path_flags_are_not_exportable(self):
        for flag in ('-ID:/repo', '-T../link.ld', '@args.rsp'):
            with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, 'Nonportable'):
                app_sdk.compiler_flags({'module_cflags': flag, 'module_ldflags': '-shared'})

    def test_windows_compiler_executable_suffix_is_equivalent(self):
        with patch.object(app_sdk, 'tool', return_value='gcc'), patch.object(
                app_sdk, 'run', return_value='arm-none-eabi-gcc.exe (Test) 14.2\n'):
            app_sdk.check_compiler({'compiler': 'arm-none-eabi-gcc (Test) 14.2'}, None)
            with self.assertRaisesRegex(ValueError, 'Compiler mismatch'):
                app_sdk.check_compiler({'compiler': 'arm-none-eabi-gcc (Test) 13.0'}, None)

    def test_main_export_table_excludes_regular_firmware_globals(self):
        with patch.object(app_sdk, 'tool', return_value='nm'), patch.object(app_sdk, 'run', return_value=
                '0100 R __rtmsymtab_start\n0110 R __rtmsymtab_end\n'
                '0100 R __rtmsym_epd_app_label\n0002 R __rtmsym_epd_app_label_name\n'
                '0108 R __rtmsym_valid_api_name\n0010 R __rtmsym_valid_api_name_name\n0003 T private_api\n'):
            self.assertEqual(app_sdk.firmware_exports('main.elf', None), ['epd_app_label', 'valid_api_name'])

    def nm(self, argv, **unused):
        if '--undefined-only' in argv:
            return '         U epd_app_label\n         U rt_snprintf\n'
        return ''.join('0001 T ' + name + '\n' for name in app_sdk.ENTRY_SYMBOLS)

    def test_import_and_entry_validation(self):
        module = self.root / 'hello.so'
        make_module(module, self.recipe['build_id'])
        with patch.object(app_sdk, 'tool', return_value='nm'), patch.object(app_sdk, 'run', side_effect=self.nm):
            report = app_sdk.check_module(self.root, module)
            self.assertEqual(report['imports'], ['epd_app_label', 'rt_snprintf'])
            self.assertEqual(report['load_bytes'], 300)
        for output, message in (('         U private_firmware_api\n', 'Unexported module imports'),
                                ('', 'Missing application entry symbols')):
            with self.subTest(message=message), patch.object(app_sdk, 'tool', return_value='nm'), \
                    patch.object(app_sdk, 'run', return_value=output):
                with self.assertRaisesRegex(ValueError, message):
                    app_sdk.check_module(self.root, module)

    def test_wrong_board_and_invalid_elf_rejected_before_nm(self):
        module = self.root / 'hello.so'
        make_module(module, '0' * 64)
        with patch.object(app_sdk, 'run', side_effect=AssertionError('nm should not run')):
            with self.assertRaisesRegex(ValueError, 'profile fingerprint mismatch'):
                app_sdk.check_module(self.root, module)
            module.write_bytes(b'not an ELF')
            with self.assertRaisesRegex(ValueError, '32-bit little-endian ELF'):
                app_sdk.check_module(self.root, module)

    def test_rejected_module_never_creates_package(self):
        app = self.root / 'app'
        app.mkdir()
        output = self.root / 'package'
        with patch.object(app_sdk, 'check_module', side_effect=ValueError('Unexported module imports')):
            with self.assertRaisesRegex(ValueError, 'Unexported'):
                app_sdk.package_app(self.root, app, self.root / 'bad.so', output)
        self.assertFalse(output.exists())

    def test_build_failure_never_publishes_partial_output(self):
        app = self.root / 'app'
        (app / 'src').mkdir(parents=True)
        (app / 'src/main.c').write_text('invalid C')
        (app / 'app.json').write_text('{"id": "hello"}')
        output = self.root / 'build'
        with patch.object(app_sdk, 'check_compiler'), patch.object(app_sdk, 'gcc', side_effect=ValueError('bad C')):
            with self.assertRaisesRegex(ValueError, 'bad C'):
                app_sdk.build_app(self.root, app, output)
        self.assertFalse(output.exists())

    def test_build_rejects_hidden_original_project_dependency(self):
        app = self.root / 'app'
        (app / 'src').mkdir(parents=True)
        (app / 'src/main.c').write_text('/* source */')
        (app / 'app.json').write_text('{"id": "hello"}')
        output = self.root / 'build'

        def preprocess(arguments, directory, toolchain):
            depfile = Path(arguments[arguments.index('-MF') + 1])
            external = self.root.parent / 'original-project/header.h'
            depfile.write_text('sdk: ' + str(external).replace(' ', '\\ '))

        with patch.object(app_sdk, 'check_compiler'), patch.object(app_sdk, 'gcc', side_effect=preprocess):
            with self.assertRaisesRegex(ValueError, 'dependency outside SDK/app'):
                app_sdk.build_app(self.root, app, output)
        self.assertFalse(output.exists())


@unittest.skipUnless(os.environ.get('EPD_APP_SDK_BUNDLES'), 'Set EPD_APP_SDK_BUNDLES for real GCC builds')
class RelocatedSDKTests(unittest.TestCase):
    def test_all_examples_build_check_and_package_without_original_repository(self):
        # The child Python audit hook forbids all access to the original repo.
        # GCC receives only SDK-relative includes and a cleared SDK environment.
        original = str(Path(__file__).resolve().parents[2])
        bootstrap = ('import os,runpy,sys; root=os.path.normcase(os.path.abspath(sys.argv.pop(1))); '
                     'script=sys.argv[1]; sys.path.insert(0,os.path.dirname(script)); '
                     'exec("def audit(event,args):\\n'
                     ' if event in (\'open\',\'os.listdir\',\'os.scandir\') and args and isinstance(args[0],(str,bytes)):\\n'
                     '  p=os.path.normcase(os.path.abspath(os.fsdecode(args[0])))\\n'
                     '  if p==root or p.startswith(root+os.sep): raise RuntimeError(\'Original repo access: \'+p)\\n"); '
                     'sys.addaudithook(audit); sys.argv=sys.argv[1:]; runpy.run_path(script,run_name="__main__")')
        with tempfile.TemporaryDirectory(prefix='app sdk relocated ') as temp:
            temp = Path(temp)
            built = []
            for index, source in enumerate(os.environ['EPD_APP_SDK_BUNDLES'].split(os.pathsep)):
                sdk = temp / f'board {index}'
                shutil.copytree(source, sdk)
                recipe = app_sdk.load_sdk(sdk)
                self.assertNotIn(original.replace('\\', '/').lower(),
                                 (sdk / 'sdk.json').read_text().replace('\\', '/').lower())

                def cli(*arguments, fail=False):
                    completed = subprocess.run([sys.executable, '-B', '-c', bootstrap, original,
                                                str(sdk / 'app_sdk.py'), *map(str, arguments)],
                                               cwd=temp, env=app_sdk.child_environment(),
                                               text=True, capture_output=True)
                    if fail:
                        self.assertNotEqual(completed.returncode, 0, completed.stdout)
                        return completed.stderr
                    self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                    return json.loads(completed.stdout)

                cli('check-sdk')
                for example in ('hello', 'netdemo', 'cfgdemo'):
                    with self.subTest(board=recipe['board'], example=example):
                        app, output = temp / f'app {index} {example}', temp / f'build {index} {example}'
                        cli('init', '--example', example, '--output', app)
                        report = cli('build', '--app', app, '--output', output)
                        module = output / (example + '.so')
                        self.assertEqual(report['build_id'], recipe['build_id'])
                        self.assertGreater(len(report['imports']), 0)
                        self.assertEqual(cli('check', '--module', module)['module_sha256'], app_sdk.digest(module))
                        package = temp / f'package {index} {example}'
                        cli('package', '--app', app, '--module', module, '--output', package)
                        metadata = json.loads((package / 'app.json').read_text())
                        self.assertEqual(metadata['build_id'], recipe['build_id'])
                        self.assertEqual(metadata['files_sha256'], app_sdk.digest(package / 'files.sha256'))
                        self.assertEqual((package / (example + '.so')).read_bytes(), module.read_bytes())
                        if example == 'hello':
                            built.append((sdk, module, recipe['build_id']))
                # Force a live reference to a symbol missing from the firmware.
                source = app / 'src/app_main.c'
                source.write_text(source.read_text().replace('(void)intent;',
                    'extern void sdk_missing_import(void); sdk_missing_import(); (void)intent;'))
                error = cli('build', '--app', app, '--output', temp / f'bad {index}', fail=True)
                self.assertIn('sdk_missing_import', error)
            if len(built) >= 2 and built[0][2] != built[1][2]:
                with self.assertRaisesRegex(ValueError, 'profile fingerprint mismatch'):
                    app_sdk.check_module(built[1][0], built[0][1])


if __name__ == '__main__':
    unittest.main()
