"""Build native modules and their independent EZIP application packages."""
import json
import re
from pathlib import Path
import struct
import subprocess
import sys
import shutil
import tempfile
from package_app import build_package
from check_app_imports import verify
from prebuilt_dictionary import unpack


SYMBOLS = ('location', 'humidity', 'wind', 'visibility', 'cloud',
           'sunrise', 'sunset', 'pressure', 'air')


def select_apps(value):
    """Resolve an explicit selection without reading any application sources."""
    names = [name.strip() for name in value.split(',') if name.strip()]
    if not names or names == ['none']:
        return []
    if names == ['all']:
        return list(PACKAGE_BUILDERS)
    unknown = set(names) - PACKAGE_BUILDERS.keys()
    if unknown:
        raise ValueError('Unknown APPS/PREINSTALL: ' + ', '.join(sorted(unknown)) +
                         '; choose ' + ','.join(PACKAGE_BUILDERS) + ', all or none')
    return list(dict.fromkeys(names))


def add_selected_packages(env, sdk, repo, names, dictionary=None):
    if not names:
        return {}
    repo = Path(repo).resolve()
    for name in names:
        for source in ('SConstruct', 'app.json'):
            path = repo / 'modules' / name / source
            if not path.is_file():
                raise ValueError('Selected application source is missing: ' + str(path))
    config = (Path(env['build_dir']) / 'rtconfig.h').read_text(encoding='utf-8')
    defines = dict(re.findall(r'^#define[ \t]+(\w+)[ \t]*([^\r\n]*)', config, re.MULTILINE))
    rgb565 = defines.get('LV_COLOR_DEPTH', '').strip() == '16'
    palette = (1 if 'EZIP_PAL_SUPPORT_1' in defines else 0) if 'EZIP_PAL_SUPPORT' in defines else None
    return {name: PACKAGE_BUILDERS[name](env, sdk, repo, rgb565, palette,
                                        **({'dictionary': dictionary} if name == 'words' else {}))
            for name in names}


def add_package(env, sdk, repo, app_id, inputs, rgb565=True, palette=None, resources=None,
                libraries=None):
    from SCons.Script import Action, Copy, Value
    repo, sdk = Path(repo).resolve(), Path(sdk).resolve()
    build = Path(env['build_dir']).resolve()
    root = build / ('app-resources/' + app_id)
    image_root = build / (app_id + '-images')
    module_dir = repo / ('modules/' + app_id)
    module = module_dir / ('output/' + app_id + '.so')
    library_projects = [(module_dir / directory, name) for directory, name in (libraries or ())]
    library_modules = [directory / ('output/' + name + '.so') for directory, name in library_projects]
    tool = sdk / 'tools/png2ezip/ezip.exe'
    flags = ['-rgb565' if rgb565 else '-rgb888', '-lvgl_version', '9',
             '-binfile', '2', '-binext', '.ezip', '-dpt', '1', '-chip', 'sf57x']
    if palette is not None:
        flags += ['-pal_support'] + (['1'] if palette == 1 else [])

    def convert(target, source, env):
        source, target = Path(str(source[0])).resolve(), Path(str(target[0])).resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([str(tool), '-convert', str(source), *flags,
                                 '-outdir', str(target.parent)], capture_output=True, text=True)
        generated = target.parent / (source.stem + '.ezip')
        if result.returncode or not generated.is_file():
            raise RuntimeError(result.stdout + result.stderr)
        if generated != target:
            generated.replace(target)
        data = target.read_bytes()
        png = source.read_bytes()
        width, height = struct.unpack_from('>II', png, 16)
        magic, cf, image_flags, w, h, stride, reserved = struct.unpack_from('<BB5H', data)
        if len(data) <= 12 or magic != 0x19 or not image_flags & 0x100 or (w, h) != (width, height):
            raise ValueError('Invalid SDK LVGL V9 EZIP file: ' + str(target))
        return 0

    images = []
    for name, source in inputs.items():
        node = env.Command(str(image_root / name), [str(source), str(tool), __file__, Value(flags)],
                           Action(convert, 'EZIP $TARGET'))
        images.extend(node)
    resource_names = []
    for name, source in (resources or {}).items():
        name = 'res/' + name
        resource_names.append(name)
        images.extend(env.Command(str(image_root / name), str(source), Copy('$TARGET', '$SOURCE')))
    asset_names = sorted(list(inputs) + resource_names)
    profile = build / 'app-profile.json'

    def compile_module(target, source, env):
        for directory, name in library_projects:
            subprocess.run([sys.executable, '-m', 'SCons', '-C', str(directory),
                            'FIRMWARE=' + str(build), '-j' + str(env.get('APP_JOBS', 8))], check=True)
        subprocess.run([sys.executable, '-m', 'SCons', '-C', str(module_dir),
                        'FIRMWARE=' + str(build), '-j' + str(env.get('APP_JOBS', 8))], check=True)
        return 0

    module_inputs = [str(p) for p in sorted(module_dir.rglob('*')) if p.is_file() and
                     not any(part in ('output', 'packages', 'tests', 'node_modules', '__pycache__')
                             for part in p.relative_to(module_dir).parts) and
                     (p.suffix in ('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.inc', '.py', '.map') or
                      p.name in ('SConstruct', 'SConscript'))]
    module_inputs += [str(profile), str(build / 'epd_app_profile.h'), str(build / 'rtconfig.h'),
                      str(build / 'cconfig.h'), str(build / 'rtua.py'), __file__]
    if app_id in ('books', 'gallery'):
        module_inputs += [str(p) for directory in ('modules/common/image', 'modules/gallery/third_party')
                          for p in sorted((repo / directory).rglob('*')) if p.is_file() and
                          (p.suffix in ('.c', '.h', '.py') or p.name == 'SConscript')]
    module_node = env.Command([str(module)] + [str(path) for path in library_modules], module_inputs,
                              Action(compile_module, 'MODULE ' + app_id + '.so'))

    def manifest(target, source, env):
        for compiled in [module] + library_modules:
            verify(compiled, build / 'main.elf', env['APP_NM'])
        root.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=app_id + '-', dir=root.parent) as temporary:
            staged = Path(temporary) / app_id
            resources = Path(temporary) / 'res'
            for name in asset_names:
                if name.startswith('res/'):
                    destination = resources / name[4:]
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(image_root / name, destination)
            result = build_package(module_dir / 'app.json', module, profile, staged,
                                   resources if resources.exists() else None,
                                   image_root / 'icon.ezip', library_modules)
            # Replace this generated package, so removed codecs/assets cannot survive.
            previous = Path(temporary) / 'previous'
            if root.exists():
                root.rename(previous)
            try:
                staged.rename(root)
            except OSError:
                if previous.exists():
                    previous.rename(root)
                raise
        result['package'] = str(root)
        print(app_id + ' application: ' + json.dumps(result, ensure_ascii=False))
        return 0

    outputs = [str(root / 'app.json'), str(root / 'files.sha256'), str(root / (app_id + '.so'))]
    outputs += [str(root / name) for name in asset_names]
    outputs += [str(root / 'codecs' / path.name) for path in library_modules]
    package = env.Command(outputs, images + module_node +
                          [str(profile), str(build / 'main.elf'), str(module_dir / 'app.json'), __file__,
                           str(Path(__file__).with_name('package_app.py')),
                           str(Path(__file__).with_name('check_app_imports.py')), Value(asset_names)],
                          Action(manifest, app_id + ' application package'))
    # The action publishes a complete directory; retain the previous package on failure.
    env.Precious(package)
    return root, package


def add_weather_package(env, sdk, repo, rgb565=True, palette=None):
    repo = Path(repo).resolve()
    inputs = {f'res/{p.stem}.ezip': p for p in sorted((repo / 'assets/ezip/weather').glob('w*.png'))}
    if len(inputs) != 140 or 'res/w999_160.ezip' not in inputs or 'res/w999_48.ezip' not in inputs:
        raise ValueError('The weather library must contain 70 codes at 160/48 pixels')
    inputs.update({f'res/ui_icon_{name}.ezip': repo / f'assets/ezip/icons/ui_icon_{name}.png'
                   for name in SYMBOLS})
    inputs['icon.ezip'] = repo / 'assets/ezip/icons/ui_icon_weather.png'

    return add_package(env, sdk, repo, 'weather', inputs, rgb565, palette)


def add_books_package(env, sdk, repo, rgb565=True, palette=None):
    repo = Path(repo).resolve()
    inputs = {'icon.ezip': repo / 'assets/ezip/icons/ui_icon_bookshelf.png'}
    licenses = {
        'licenses/libmobi.txt': 'modules/books/third_party/libmobi/COPYING',
        'licenses/libmobi-gpl3.txt': 'modules/books/third_party/libmobi/COPYING.GPL3',
        'licenses/libxml2.txt': 'modules/books/third_party/libxml2/Copyright',
        'licenses/miniz.txt': 'modules/books/LICENSE.miniz',
        'licenses/libpng.txt': 'modules/gallery/third_party/libpng/LICENSE',
        'licenses/zlib.txt': 'modules/gallery/third_party/zlib/LICENSE',
        'licenses/tjpgd.txt': 'modules/common/image/LICENSE.tjpgd',
    }
    return add_package(env, sdk, repo, 'books', inputs, rgb565, palette,
                       resources={name: repo / path for name, path in licenses.items()},
                       libraries=[('codecs/mobi', 'bk_mobi')])


def add_gallery_package(env, sdk, repo, rgb565=True, palette=None):
    repo = Path(repo).resolve()
    inputs = {'icon.ezip': repo / 'assets/ezip/icons/ui_icon_album.png'}
    return add_package(env, sdk, repo, 'gallery', inputs, rgb565, palette)


def add_words_package(env, sdk, repo, rgb565=True, palette=None, dictionary=None):
    from SCons.Script import Action
    repo = Path(repo).resolve()
    module_dir = repo / 'modules/words'
    renderer = module_dir / 'tools/render_icon.cjs'
    icon = module_dir / 'output/assets/icon.png'

    def render_icon(target, source, env):
        subprocess.run(['node', str(renderer)], cwd=module_dir, check=True)
        return 0

    env.Command(str(icon), [str(module_dir / 'assets/icon.svg'), str(renderer)],
                Action(render_icon, 'Render words icon'))
    archive = None
    if dictionary:
        dictionary = Path(dictionary).resolve()
        report = dictionary.with_suffix('.wdb.json')
        sources = (dictionary, report)
    else:
        archive = module_dir / 'assets/dictionary/library.wdb.gz'
        report = module_dir / 'assets/dictionary/library.wdb.json'
        dictionary = Path(env['build_dir']).resolve() / 'words-dictionary/library.wdb'
        sources = (archive, report)
    for source in sources:
        if not source.is_file():
            raise ValueError('words requires dictionary and report: ' + str(source) +
                             '; see modules/words/README.md or set WORDS_DICTIONARY=<file.wdb>')
    if archive is not None:
        def restore_dictionary(target, source, env):
            unpack(str(source[0]), str(source[1]), str(target[0]))
            return 0

        restored = env.Command(str(dictionary), [str(archive), str(report),
                                                str(Path(__file__).with_name('prebuilt_dictionary.py'))],
                               Action(restore_dictionary, 'Restore words dictionary'))
        env.Precious(restored)
    resources = {'library.wdb': dictionary,
                 'library.wdb.json': report,
                 'LICENSE.ecdict': module_dir / 'LICENSE.ecdict',
                 'LICENSE.fsrs': module_dir / 'LICENSE.fsrs',
                 'SOURCES.txt': module_dir / 'SOURCES.txt'}
    return add_package(env, sdk, repo, 'words', {'icon.ezip': icon}, rgb565, palette, resources)


PACKAGE_BUILDERS = {'weather': add_weather_package, 'books': add_books_package,
                    'gallery': add_gallery_package, 'words': add_words_package}
