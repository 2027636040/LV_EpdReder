"""Export and use a relocatable, firmware-specific native application SDK."""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


FORMAT = 1
CONFIG_FILES = ('rtconfig.h', 'cconfig.h', 'epd_app_profile.h', 'app-profile.json')
MODULE_DEFINES = ['BUILD_DLMODULE', 'HAVE_CCONFIG_H']
ENTRY_SYMBOLS = {'app_main', 'epd_app_info', 'epd_app_build_id'}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n',
                          encoding='utf-8')


def child_environment():
    env = dict(os.environ)
    for key in ('SIFLI_SDK', 'SIFLI_SDK_PATH', 'RTT_ROOT', 'BSP_ROOT', 'PYTHONPATH',
                'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
                'GCC_EXEC_PREFIX', 'COMPILER_PATH'):
        env.pop(key, None)
    return env


def tool(name, toolchain=None):
    program = 'arm-none-eabi-' + name
    found = shutil.which(program, path=str(toolchain) if toolchain else None)
    if not found:
        raise ValueError(f'Missing {program}; activate export.ps1 or pass --toolchain <bin>')
    return str(Path(found).resolve())


def run(argv, cwd=None):
    result = subprocess.run([str(arg) for arg in argv], cwd=cwd, env=child_environment(),
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise ValueError(f'{Path(argv[0]).name} failed ({result.returncode}):\n'
                         + result.stdout + result.stderr)
    if result.stderr:
        print(result.stderr, end='', file=sys.stderr)
    return result.stdout


def gcc(arguments, directory, toolchain=None):
    # GCC response files avoid Windows' command line limit for the header probe.
    response = Path(directory) / 'gcc.rsp'
    response.write_text('\n'.join('"' + str(arg).replace('\\', '\\\\').replace('"', '\\"')
                                  + '"' for arg in arguments) + '\n', encoding='utf-8')
    return run([tool('gcc', toolchain), '@' + str(response)], cwd=directory)


def read_ua(path, project, rtt):
    """Read the generated SDK lists without executing rtua.py."""
    tree = ast.parse(Path(path).read_text(encoding='utf-8'))
    names = {'BSP_ROOT': str(project), 'RTT_ROOT': str(rtt)}

    def value(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return node.value
        if isinstance(node, (ast.List, ast.Tuple)):
            return [value(item) for item in node.elts]
        if isinstance(node, ast.Name) and node.id in names:
            return names[node.id]
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            return value(node.left) + value(node.right)
        raise ValueError('Unsupported generated rtua expression')

    result = []
    for function, variable in (('GetCPPPATH', 'CPPPATH'), ('GetCPPDEFINES', 'CPPDEFINES')):
        functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == function]
        if len(functions) != 1:
            raise ValueError('Missing or duplicate rtua function: ' + function)
        assignments = [n for n in functions[0].body if isinstance(n, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id == variable for t in n.targets)]
        if len(assignments) != 1:
            raise ValueError('Missing or duplicate rtua list: ' + variable)
        items = value(assignments[0].value)
        if items == '':
            items = []
        if not isinstance(items, list):
            raise ValueError('Invalid rtua list: ' + variable)
        result.append(items)
    return result


def safe_relative(name):
    if (not isinstance(name, str) or not name or '\\' in name or ':' in name
            or name.startswith('/') or any(p in ('', '.', '..') for p in name.split('/'))):
        raise ValueError('Invalid SDK relative path: ' + repr(name))
    return name


def sdk_path(root, name):
    path = (Path(root) / safe_relative(name)).resolve()
    if not path.is_relative_to(Path(root).resolve()):
        raise ValueError('SDK path escapes its root: ' + name)
    return path


def read_profile(directory):
    directory = Path(directory)
    profile = json.loads((directory / 'app-profile.json').read_text(encoding='utf-8'))
    identity = profile['build_id']
    payload = {key: val for key, val in profile.items() if key != 'build_id'}
    expected = hashlib.sha256(json.dumps(payload, sort_keys=True, ensure_ascii=True).encode()).hexdigest()
    if profile.get('profile_format') != 1 or identity != expected:
        raise ValueError('Invalid firmware profile fingerprint')
    for name in ('rtconfig.h', 'cconfig.h'):
        if digest(directory / name) != profile['configuration'][name]:
            raise ValueError('Firmware configuration fingerprint mismatch: ' + name)
    header = (directory / 'epd_app_profile.h').read_text(encoding='utf-8')
    if not re.search(r'^#define\s+EPD_APP_BUILD_ID\s+"' + re.escape(identity) + r'"\s*$',
                     header, re.MULTILINE):
        raise ValueError('epd_app_profile.h fingerprint mismatch')
    return profile


def compiler_flags(profile):
    flags = [shlex.split(profile[key]) for key in ('module_cflags', 'module_ldflags')]
    # A portable recipe must not silently retain a linker script or source path.
    for flag in flags[0] + flags[1]:
        if '/' in flag or '\\' in flag or ':' in flag or flag.startswith('@'):
            raise ValueError('Nonportable module flag: ' + flag)
    return flags


def check_compiler(profile, toolchain):
    version = run([tool('gcc', toolchain), '--version']).splitlines()[0]
    if version.replace('arm-none-eabi-gcc.exe ', 'arm-none-eabi-gcc ', 1) != profile['compiler'].replace(
            'arm-none-eabi-gcc.exe ', 'arm-none-eabi-gcc ', 1):
        raise ValueError(f'Compiler mismatch: expected {profile["compiler"]!r}, got {version!r}')


def definitions(items):
    return ['-D' + ('='.join(item) if isinstance(item, list) else item) for item in items]


def dependencies(text):
    """Parse GCC's -MT sdk dependency rule, including escaped spaces and drives."""
    flat = text.replace('\\\n', ' ').replace('\\\r\n', ' ')
    if not flat.startswith('sdk:'):
        raise ValueError('Invalid GCC dependency output')
    words = re.findall(r'(?:\\[ \t#\\]|[^\s])+', flat[4:])
    return [Path(re.sub(r'\\([ \t#\\])', r'\1', p).replace('$$', '$')).resolve() for p in words]


def firmware_exports(firmware, toolchain):
    table = {}
    for line in run([tool('nm', toolchain), firmware]).splitlines():
        fields = line.split()
        if len(fields) >= 3:
            table[fields[-1]] = int(fields[0], 16)
    if '__rtmsymtab_start' not in table or '__rtmsymtab_end' not in table:
        raise ValueError('Firmware has no RT-Thread export table boundaries')
    start, end = table['__rtmsymtab_start'], table['__rtmsymtab_end']
    # Name strings live outside this range. An exported API can itself end in _name.
    symbols = {name[len('__rtmsym_'):] for name, address in table.items()
               if name.startswith('__rtmsym_') and start <= address < end}
    if not symbols:
        raise ValueError('Firmware has no RT-Thread module exports')
    return sorted(symbols)


def export_sdk(repo, firmware, output, toolchain=None, headers=()):
    repo, firmware, output = (Path(p).resolve() for p in (repo, firmware, output))
    if output.exists():
        raise ValueError('SDK output must be a new directory: ' + str(output))
    profile = read_profile(firmware)
    check_compiler(profile, toolchain)
    cflags, ldflags = compiler_flags(profile)
    image = firmware / 'main.elf'
    if profile['build_id'].encode() + b'\0' not in image.read_bytes():
        raise ValueError('main.elf does not contain this firmware profile fingerprint')
    exports = firmware_exports(image, toolchain)
    sdk = (repo / 'SiFli-SDK').resolve()
    paths, defines = read_ua(firmware / 'rtua.py', repo / 'project', sdk / 'rtos/rtthread')
    paths = [firmware] + [Path(p).resolve() for p in paths]
    # Match the module SConstruct fallback for SCons variant header directories.
    paths += [sdk / p.as_posix().split('/sifli_sdk/', 1)[1]
              for p in paths if '/sifli_sdk/' in p.as_posix()]
    paths = list(dict.fromkeys(p.resolve() for p in paths if p.is_dir()))
    defines += MODULE_DEFINES
    public = []
    for name, expected in profile['platform_interface'].items():
        if not name.endswith('.h'):
            continue
        candidates = list((repo / 'src').rglob(name))
        if len(candidates) != 1 or digest(candidates[0]) != expected:
            raise ValueError('Public header differs from firmware profile; rebuild platform: ' + name)
        public.append(candidates[0])
    for name in headers:
        candidate = (repo / name).resolve()
        if not candidate.is_file():
            candidate = next((p / name for p in paths if (p / name).is_file()), candidate)
        if not candidate.is_file():
            raise ValueError('Public header not found: ' + name)
        public.append(candidate)
    if not public:
        raise ValueError('Firmware profile has no public headers')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='app-sdk-', dir=output.parent) as temp:
        temp = Path(temp)
        staged = temp / 'sdk'
        staged.mkdir()
        probe = temp / 'public.c'
        probe.write_text(''.join('#include "' + p.as_posix() + '"\n' for p in public), encoding='utf-8')
        depfile = temp / 'public.d'
        gcc(cflags + definitions(defines) + ['-I' + p.as_posix() for p in paths]
            + ['-E', '-MMD', '-MF', str(depfile), '-MT', 'sdk', str(probe), '-o', os.devnull],
            temp, toolchain)
        needed = set(dependencies(depfile.read_text(encoding='utf-8'))) - {probe}
        needed.update(firmware / name for name in CONFIG_FILES if name.endswith('.h'))

        def relative(path):
            path = path.resolve()
            if path.is_relative_to(sdk):
                return 'include/SiFli-SDK/' + path.relative_to(sdk).as_posix()
            if path.is_relative_to(repo):
                return 'include/' + path.relative_to(repo).as_posix()
            if path == firmware:
                return 'include/firmware'
            if path.is_relative_to(firmware):
                return 'include/firmware/' + path.relative_to(firmware).as_posix()
            raise ValueError('Dependency outside repository/firmware: ' + str(path))

        for source in sorted(needed):
            target = staged / relative(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        (staged / 'config').mkdir()
        for name in CONFIG_FILES:
            shutil.copyfile(firmware / name, staged / 'config' / name)
        for source in public:
            expected = profile['platform_interface'].get(source.name)
            if expected and digest(staged / relative(source)) != expected:
                raise ValueError('Public header changed during export: ' + source.name)
        includes = [relative(p) for p in paths if any(n.is_relative_to(p) for n in needed)]
        includes = list(dict.fromkeys(['config'] + includes))
        (staged / 'firmware-exports.txt').write_text(''.join(s + '\n' for s in exports), encoding='utf-8')
        shutil.copyfile(__file__, staged / 'app_sdk.py')
        shutil.copyfile(Path(__file__).with_name('package_app.py'), staged / 'package_app.py')
        shutil.copytree(Path(__file__).with_name('app_sdk_template'), staged / 'examples',
                        ignore=shutil.ignore_patterns('__pycache__', 'output', '*.pyc'))
        shutil.copyfile(repo / 'docs/APP_SDK.md', staged / 'README.md')
        recipe = {'sdk_format': FORMAT, 'board': profile['board'], 'build_id': profile['build_id'],
                  'compiler': profile['compiler'], 'cflags': cflags,
                  'ldflags': ldflags + ['-e', '0'],
                  'strip_flags': ['-R', '.hash'],
                  'defines': defines, 'include_paths': includes,
                  'public_headers': [relative(p) for p in public],
                  'firmware_sha256': digest(image), 'rtua_sha256': digest(firmware / 'rtua.py'),
                  'export_count': len(exports)}
        # Store the evaluated rtua lists as relative data, never original paths.
        write_json(staged / 'rtua.json', {'CPPPATH': includes, 'CPPDEFINES': defines})
        recipe['files'] = {p.relative_to(staged).as_posix(): digest(p)
                           for p in sorted(staged.rglob('*')) if p.is_file()}
        write_json(staged / 'sdk.json', recipe)
        load_sdk(staged)
        staged.rename(output)
    return {'sdk': str(output), 'board': profile['board'], 'build_id': profile['build_id'],
            'headers': len([p for p in needed if p.suffix == '.h']), 'exports': len(exports)}


def load_sdk(root):
    root = Path(root).resolve()
    recipe = json.loads((root / 'sdk.json').read_text(encoding='utf-8'))
    if recipe.get('sdk_format') != FORMAT:
        raise ValueError('Unsupported SDK format')
    for name, expected in recipe['files'].items():
        if digest(sdk_path(root, name)) != expected:
            raise ValueError('SDK file checksum mismatch: ' + name)
    profile = read_profile(root / 'config')
    cflags, ldflags = compiler_flags(profile)
    if (recipe['build_id'] != profile['build_id'] or recipe['board'] != profile['board']
            or recipe['compiler'] != profile['compiler'] or recipe['cflags'] != cflags
            or recipe['ldflags'] != ldflags + ['-e', '0']):
        raise ValueError('SDK recipe/profile mismatch')
    ua = json.loads((root / 'rtua.json').read_text(encoding='utf-8'))
    if ua != {'CPPPATH': recipe['include_paths'], 'CPPDEFINES': recipe['defines']}:
        raise ValueError('SDK recipe/rtua mismatch')
    for name in recipe['include_paths']:
        if not sdk_path(root, name).is_dir():
            raise ValueError('Missing SDK include directory: ' + name)
    return recipe


def check_module(root, module, toolchain=None):
    from package_app import elf_load_bytes
    root, module = Path(root).resolve(), Path(module).resolve()
    recipe = load_sdk(root)
    data = module.read_bytes()
    load_bytes = elf_load_bytes(data)
    if ('EPDAPP:' + recipe['build_id']).encode() + b'\0' not in data:
        raise ValueError('Module firmware profile fingerprint mismatch')
    imports = {line.split()[-1] for line in run(
        [tool('nm', toolchain), '-D', '--undefined-only', module]).splitlines() if line.split()}
    exports = set((root / 'firmware-exports.txt').read_text(encoding='utf-8').splitlines())
    missing = sorted(imports - exports)
    if missing:
        raise ValueError('Unexported module imports: ' + ', '.join(missing))
    defined = {line.split()[-1] for line in run(
        [tool('nm', toolchain), '-D', '--defined-only', module]).splitlines() if line.split()}
    if not ENTRY_SYMBOLS <= defined:
        raise ValueError('Missing application entry symbols: ' + ', '.join(sorted(ENTRY_SYMBOLS - defined)))
    return {'module': str(module), 'build_id': recipe['build_id'], 'imports': sorted(imports),
            'module_sha256': digest(module), 'load_bytes': load_bytes}


def build_app(root, app, output, toolchain=None):
    root, app, output = (Path(p).resolve() for p in (root, app, output))
    recipe = load_sdk(root)
    check_compiler(recipe, toolchain)
    metadata = json.loads((app / 'app.json').read_text(encoding='utf-8'))
    app_id = metadata.get('id', '')
    if not isinstance(app_id, str) or not re.fullmatch('[a-z0-9_]{1,7}', app_id):
        raise ValueError('Application id must contain 1-7 lowercase letters, digits or underscores')
    sources = sorted((app / 'src').rglob('*.c'))
    if not sources:
        raise ValueError('Application requires C sources under src/')
    if output.exists():
        raise ValueError('Build output must be a new directory: ' + str(output))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='app-build-', dir=output.parent) as temp:
        temp = Path(temp)
        objects = []
        includes = [sdk_path(root, p) for p in recipe['include_paths']] + [app / 'src', app / 'include']
        for index, source in enumerate(sources):
            obj = temp / f'{index}.o'
            depfile = temp / f'{index}.d'
            gcc(recipe['cflags'] + definitions(recipe['defines'])
                + ['-I' + p.as_posix() for p in includes]
                + ['-MMD', '-MF', str(depfile), '-MT', 'sdk', '-c', str(source), '-o', str(obj)],
                temp, toolchain)
            for dependency in dependencies(depfile.read_text(encoding='utf-8')):
                if not dependency.is_relative_to(root) and not dependency.is_relative_to(app):
                    raise ValueError('Application dependency outside SDK/app: ' + str(dependency))
            objects.append(str(obj))
        unstripped = temp / (app_id + '.so.nostrip')
        gcc(recipe['ldflags'] + objects + ['-o', str(unstripped)], temp, toolchain)
        module = temp / (app_id + '.so')
        run([tool('strip', toolchain), *recipe['strip_flags'], '-o', module, unstripped])
        report = check_module(root, module, toolchain)
        staged = temp / 'result'
        staged.mkdir()
        for source in (module, unstripped):
            shutil.copyfile(source, staged / source.name)
        report['module'] = str(output / module.name)
        write_json(staged / 'build-report.json', report)
        staged.rename(output)
    return report


def package_app(root, app, module, output, toolchain=None):
    from package_app import build_package
    root, app, module = (Path(p).resolve() for p in (root, app, module))
    check_module(root, module, toolchain)
    resources, icon = app / 'res', app / 'icon.ezip'
    return build_package(app / 'app.json', module, root / 'config/app-profile.json', output,
                         resources if resources.is_dir() else None, icon if icon.is_file() else None)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    export = sub.add_parser('export', help='Export from an existing firmware build')
    export.add_argument('--repo', default=str(Path(__file__).resolve().parents[1]))
    export.add_argument('--firmware', required=True)
    export.add_argument('--output', required=True)
    export.add_argument('--header', action='append', default=[], help='Additional public header to collect')
    for name in ('check-sdk', 'init', 'build', 'check', 'package'):
        child = sub.add_parser(name)
        child.add_argument('--sdk', default=str(Path(__file__).resolve().parent))
        if name in ('init', 'build', 'package'):
            child.add_argument('--output', required=True)
        if name == 'init':
            child.add_argument('--example', choices=('hello', 'netdemo', 'cfgdemo'), default='hello')
        if name in ('build', 'package'):
            child.add_argument('--app', required=True)
        if name in ('check', 'package'):
            child.add_argument('--module', required=True)
        if name in ('build', 'check', 'package'):
            child.add_argument('--toolchain', help='Arm GNU toolchain bin directory')
    export.add_argument('--toolchain')
    args = parser.parse_args(argv)
    try:
        if args.command == 'export':
            result = export_sdk(args.repo, args.firmware, args.output, args.toolchain, args.header)
        elif args.command == 'check-sdk':
            recipe = load_sdk(args.sdk)
            result = {key: recipe[key] for key in ('board', 'build_id', 'export_count')}
        elif args.command == 'init':
            load_sdk(args.sdk)
            shutil.copytree(Path(args.sdk) / 'examples' / args.example, args.output)
            result = {'app': str(Path(args.output).resolve())}
        elif args.command == 'build':
            result = build_app(args.sdk, args.app, args.output, args.toolchain)
        elif args.command == 'check':
            result = check_module(args.sdk, args.module, args.toolchain)
        else:
            result = package_app(args.sdk, args.app, args.module, args.output, args.toolchain)
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except (ValueError, OSError, KeyError, TypeError, SyntaxError) as error:
        parser.exit(1, f'App SDK failed: {error}\n')


if __name__ == '__main__':
    main()
