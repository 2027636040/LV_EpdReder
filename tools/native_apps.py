"""Build native modules and their independent EZIP application packages."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import shutil
import tempfile
from package_app import build_package
from check_app_imports import verify


SYMBOLS = ('location', 'humidity', 'wind', 'visibility', 'cloud',
           'sunrise', 'sunset', 'pressure', 'air')


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
                            'FIRMWARE=' + build.name, '-j8'], check=True)
        subprocess.run([sys.executable, '-m', 'SCons', '-C', str(module_dir),
                        'FIRMWARE=' + build.name, '-j8'], check=True)
        return 0

    module_inputs = [str(p) for p in sorted(module_dir.rglob('*')) if p.is_file() and
                     not any(part in ('output', 'packages', 'tests', 'node_modules', '__pycache__')
                             for part in p.relative_to(module_dir).parts) and
                     (p.suffix in ('.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.inc', '.py', '.map') or
                      p.name in ('SConstruct', 'SConscript'))]
    module_inputs += [str(profile), str(build / 'epd_app_profile.h'), str(build / 'rtconfig.h'),
                      str(build / 'cconfig.h'), str(repo / 'project/rtua.py'), __file__]
    if app_id in ('books', 'gallery'):
        module_inputs += [str(p) for directory in ('modules/common/image', 'modules/gallery/third_party')
                          for p in sorted((repo / directory).rglob('*')) if p.is_file() and
                          (p.suffix in ('.c', '.h', '.py') or p.name == 'SConscript')]
    module_node = env.Command([str(module)] + [str(path) for path in library_modules], module_inputs,
                              Action(compile_module, 'MODULE ' + app_id + '.so'))

    def manifest(target, source, env):
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
                          [str(profile), str(module_dir / 'app.json'), __file__,
                           str(Path(__file__).with_name('package_app.py')), Value(asset_names)],
                          Action(manifest, app_id + ' application package'))
    def check_imports(target, source, env):
        import rtconfig
        nm = Path(rtconfig.EXEC_PATH) / 'arm-none-eabi-nm.exe'
        verify(module, build / 'main.elf', nm)
        for library in library_modules:
            verify(library, build / 'main.elf', nm)
        return 0

    env.AddPostAction(env['target'], Action(check_imports, 'Check ' + app_id + ' module imports'))
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
    dictionary = Path(dictionary).resolve() if dictionary else module_dir / 'output/dictionaries/cet4.wdb'
    resources = {'library.wdb': dictionary,
                 'library.wdb.json': dictionary.with_suffix('.wdb.json'),
                 'LICENSE.ecdict': module_dir / 'LICENSE.ecdict',
                 'LICENSE.fsrs': module_dir / 'LICENSE.fsrs'}
    return add_package(env, sdk, repo, 'words', {'icon.ezip': icon}, rgb565, palette, resources)
