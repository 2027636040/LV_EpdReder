"""Archive exact GNU symbols and explicitly export reproducible debug inputs.

This is build tooling, not a dump reader or a stack analyser.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

GIT = shutil.which('git') or 'git'


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(*args):
    command = [str(arg) for arg in args]
    if command[0] == 'git':
        command[0] = GIT
    try:
        return subprocess.check_output(command)
    except FileNotFoundError as error:
        raise RuntimeError('Cannot execute build tool: ' + command[0]) from error


def build_id(path, readelf):
    notes = run(readelf, '-nW', path).decode('utf-8', errors='replace')
    identities = re.findall(r'Build ID:\s*([0-9a-fA-F]+)', notes)
    if len(identities) != 1 or len(identities[0]) != 40:
        raise ValueError('Expected one GNU SHA-1 build-id in ' + str(path))
    return identities[0].lower()


def source_diff(root):
    # Documentation is not a compiler input. It can be edited after a build
    # without making that build's source provenance impossible to export.
    # Each submodule has its own revision/diff entry below. A synthetic
    # "Subproject commit ...-dirty" diff cannot restore its working tree.
    return run('git', '-C', root, 'diff', '--binary', '--ignore-submodules=all', 'HEAD', '--', '.',
               ':(exclude)*.md', ':(exclude)*.log')


def source_state(repo):
    """A revision plus diff and untracked input hashes; no source export here."""
    roots = {'project': repo, 'sdk': repo / 'SiFli-SDK'}
    sdk = roots['sdk']
    for line in run('git', '-C', sdk, 'submodule', 'status', '--recursive').decode().splitlines():
        name = line.strip().split()[1]
        roots['sdk/' + name] = sdk / name
    result = {}
    for name, root in roots.items():
        diff = source_diff(root)
        untracked = {}
        for relative in run('git', '-C', root, 'ls-files', '--others', '--exclude-standard', '-z').decode().split('\0'):
            if not relative:
                continue
            if Path(relative).suffix in ('.md', '.log'):
                continue
            # Local build/log archives are outputs, not reproducible source inputs.
            if (name == 'project' and (relative.startswith(('project/build_', 'log/')))):
                continue
            path = root / relative
            if path.is_file():
                untracked[relative] = sha256(path)
        result[name] = {
            'root': str(root),
            'commit': run('git', '-C', root, 'rev-parse', 'HEAD').decode().strip(),
            'dirty': bool(diff or untracked),
            'diff_sha256': hashlib.sha256(diff).hexdigest(),
            'untracked': untracked,
        }
    return result


def copy_exact(source, destination):
    """Keep earlier builds immutable, even when a module name is reused."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if sha256(source) != sha256(destination):
            raise ValueError('Conflicting contents for archived identity: ' + str(destination))
    else:
        temporary = destination.with_suffix(destination.suffix + '.tmp')
        shutil.copyfile(source, temporary)
        temporary.replace(destination)


def write_json(path, value):
    data = (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n').encode()
    if path.exists() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_bytes(data)
    temporary.replace(path)


def archive(repo, build, app_ids, readelf, fixtures=False, firmware_built=False):
    repo, build = Path(repo).resolve(), Path(build).resolve()
    root = build / 'debug-artifacts'
    manifest_path = root / 'manifest.json'
    manifest = (json.loads(manifest_path.read_text(encoding='utf-8')) if manifest_path.exists()
                else {'format': 1, 'symbols': {}, 'builds': {}})
    if manifest['format'] != 1:
        raise ValueError('Unsupported debug manifest format')
    profile = json.loads((build / 'app-profile.json').read_text(encoding='utf-8'))
    context = {'board': profile['board'], 'compiler': profile['compiler'],
               'module_cflags': profile['module_cflags'], 'module_ldflags': profile['module_ldflags'],
               'platform_abi': profile['build_id'], 'sources': source_state(repo)}

    def add_symbol(elf):
        identity = build_id(elf, readelf)
        # One immutable symbol file per identity, independent of installation path.
        relative = Path('symbols') / identity / elf.name
        copy_exact(elf, root / relative)
        record = {'file': relative.as_posix(), 'sha256': sha256(elf), 'source_elf': str(elf)}
        previous = manifest['symbols'].get(identity)
        if previous and previous['sha256'] != record['sha256']:
            raise ValueError('GNU build-id collision: ' + identity)
        manifest['symbols'][identity] = record
        return identity

    firmware_id = add_symbol(build / 'main.elf')
    firmware = manifest['builds'].get(firmware_id)
    if firmware is None:
        firmware = {'context': context, 'applications': {}, 'configuration': {}}
        manifest['builds'][firmware_id] = firmware
    if firmware_built:
        # The platform build has checked all firmware dependencies. Source-only
        # edits can leave the ELF unchanged; an apps-only build cannot make this claim.
        firmware['context'] = context
    if firmware_built or not firmware['configuration']:
        for name in ('rtconfig.h', 'cconfig.h', 'app-profile.json', 'epd_app_profile.h',
                     'rtua.py', 'link_copy.lds'):
            source = build / name
            relative = Path('symbols') / firmware_id / name
            copy_exact(source, root / relative)
            firmware['configuration'][name] = {'file': relative.as_posix(), 'sha256': sha256(source)}
    for app_id in app_ids:
        package_root = build / 'app-resources' / app_id
        package = json.loads((package_root / 'app.json').read_text(encoding='utf-8'))
        modules = []
        for installed in sorted(package_root.rglob('*.so')):
            relative = installed.relative_to(package_root)
            if relative.parts[0] == 'codecs':
                # Private library project and filename are already known to the builder.
                matches = list((repo / 'modules' / app_id / 'codecs').glob('*/output/' + installed.name + '.nostrip'))
                if len(matches) != 1:
                    raise ValueError('Cannot locate exact private-library symbols: ' + str(installed))
                symbols = matches[0]
            else:
                symbols = repo / 'modules' / app_id / 'output' / (installed.name + '.nostrip')
            identity = add_symbol(symbols)
            if identity != build_id(installed, readelf):
                raise ValueError('Install/symbol build-id mismatch: ' + str(installed))
            modules.append({'build_id': identity, 'installed_file': relative.as_posix(),
                            'installed_sha256': sha256(installed)})
        firmware['applications'][app_id] = {'version': package['version'], 'modules': modules,
                                           'context': context}
    if fixtures:
        firmware['fixtures'] = []
        for name in ('diagmod', 'diaglib'):
            symbols = repo / 'tests/diagnostics/module/output' / (name + '.so.nostrip')
            installed = build / 'diagnostic-fixtures' / (name + '.so')
            identity = add_symbol(symbols)
            if identity != build_id(installed, readelf):
                raise ValueError('Fixture install/symbol mismatch: ' + str(installed))
            firmware['fixtures'].append({'build_id': identity, 'installed_file': str(installed),
                                         'installed_sha256': sha256(installed)})
    manifest['current_firmware'] = firmware_id
    write_json(manifest_path, manifest)
    print('Debug symbols: ' + str(manifest_path))
    return manifest


def add_fixture_modules(env, repo):
    from SCons.Script import Action
    repo = Path(repo).resolve()
    build = Path(env['build_dir']).resolve()
    directory = repo / 'tests/diagnostics/module'
    targets = []
    for name in ('diagmod', 'diaglib'):
        targets += [str(directory / 'output' / (name + suffix)) for suffix in ('.so', '.so.nostrip')]
        targets.append(str(build / 'diagnostic-fixtures' / (name + '.so')))

    def compile_fixtures(target, source, env):
        for private, name in (('0', 'diagmod'), ('1', 'diaglib')):
            subprocess.run([sys.executable, '-m', 'SCons', '-C', str(directory),
                            'FIRMWARE=' + str(build), 'PRIVATE=' + private,
                            '-j' + str(env.get('APP_JOBS', 8))], check=True)
            destination = build / 'diagnostic-fixtures' / (name + '.so')
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(directory / 'output' / (name + '.so'), destination)
        return 0

    inputs = [str(path) for path in directory.parent.rglob('*') if path.is_file() and
              'output' not in path.parts and '__pycache__' not in path.parts and
              (path.suffix in ('.py', '.c', '.h') or path.name in ('SConstruct', 'SConscript'))]
    inputs += [str(build / name) for name in ('rtconfig.h', 'cconfig.h', 'rtua.py', 'app-profile.json')]
    inputs += [__file__, str(repo / 'SiFli-SDK/tools/build/ua.py'), str(repo / 'SiFli-SDK/tools/build/building.py')]
    nodes = env.Command(targets, inputs, Action(compile_fixtures, 'Build explicit fault fixtures'))
    env.Precious(nodes)
    return nodes


def add_archive(env, repo, packages, fixtures=None, firmware_built=False):
    """One final action serialises the manifest after all selected packages."""
    from SCons.Script import Action, Value
    build = Path(env['build_dir']).resolve()
    names = list(packages)
    readelf = str(Path(env['APP_NM']).with_name(Path(env['APP_NM']).name.replace('-nm', '-readelf')))

    def collect(target, source, env):
        archive(repo, build, names, readelf, bool(fixtures), firmware_built)
        return 0

    dependencies = [str(build / 'main.elf'), str(build / 'app-profile.json'), __file__, Value(names)]
    dependencies += [node for _, nodes in packages.values() for node in nodes]
    dependencies += list(fixtures or [])
    target = env.Command(str(build / 'debug-artifacts/manifest.json'), dependencies,
                         Action(collect, 'Archive exact debug symbols'))
    # Also repairs missing archive files and records current source provenance.
    env.AlwaysBuild(target)
    env.Precious(target)
    return target


def export_bundle(repo, build, output, firmware_id=None):
    """Export symbols plus Git revisions, binary diffs and untracked inputs."""
    repo, build, output = Path(repo).resolve(), Path(build).resolve(), Path(output).resolve()
    root = build / 'debug-artifacts'
    manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    identity = firmware_id or manifest['current_firmware']
    firmware = manifest['builds'][identity]
    current = source_state(repo)
    contexts = [firmware['context']] + [app['context'] for app in firmware['applications'].values()]
    for context in contexts:
        if context['sources'] != current:
            raise ValueError('Sources changed since this build. Rebuild the firmware and selected apps '
                             'before exporting source diffs; archived symbols remain available.')
    selected = {identity}
    for app in firmware['applications'].values():
        selected.update(module['build_id'] for module in app['modules'])
    selected.update(module['build_id'] for module in firmware.get('fixtures', []))
    bundle_manifest = dict(manifest, current_firmware=identity,
                           builds={identity: firmware},
                           symbols={key: manifest['symbols'][key] for key in selected})
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        raise ValueError('Debug bundle already exists: ' + str(output))
    with tempfile.TemporaryDirectory(prefix='epd-debug-', dir=output.parent) as temporary:
        temporary_zip = Path(temporary) / 'bundle.zip'
        with zipfile.ZipFile(temporary_zip, 'w', zipfile.ZIP_DEFLATED) as archive_file:
            archive_file.writestr('manifest.json', json.dumps(bundle_manifest, ensure_ascii=False, indent=2))
            records = [manifest['symbols'][key] for key in selected] + list(firmware['configuration'].values())
            for record in records:
                path = root / record['file']
                if sha256(path) != record['sha256']:
                    raise ValueError('Archived input changed: ' + str(path))
                archive_file.write(path, record['file'])
            for name, state in current.items():
                repository = Path(state['root'])
                diff = source_diff(repository)
                if hashlib.sha256(diff).hexdigest() != state['diff_sha256']:
                    raise ValueError('Source changed while exporting: ' + str(repository))
                archive_file.writestr('sources/' + name + '/changes.patch', diff)
                for relative, expected in state['untracked'].items():
                    source = repository / relative
                    if sha256(source) != expected:
                        raise ValueError('Source changed while exporting: ' + str(source))
                    archive_file.write(source, 'sources/' + name + '/untracked/' + relative)
        temporary_zip.rename(output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--firmware-id')
    args = parser.parse_args()
    print(export_bundle(Path(__file__).resolve().parents[1], args.build, args.output, args.firmware_id))


if __name__ == '__main__':
    main()
