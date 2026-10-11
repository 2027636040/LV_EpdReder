"""Validate complete native packages and generate a firmware-specific catalogue."""
import argparse
import hashlib
import json
import re
import stat
from pathlib import Path

from package_app import relative_name, sha256


def is_link(path):
    try:
        status = path.lstat()
    except FileNotFoundError:
        return False
    return (stat.S_ISLNK(status.st_mode) or
            getattr(status, 'st_reparse_tag', 0) ==
            getattr(stat, 'IO_REPARSE_TAG_MOUNT_POINT', None))


def inspect_package(root, build_id, release_root):
    root = Path(root)
    if is_link(root):
        raise ValueError(f'Linked package root: {root}')
    root = root.resolve()
    if not root.is_relative_to(Path(release_root).resolve()):
        raise ValueError(f'Package outside release root: {root}')
    manifest = root / 'app.json'
    manifest_bytes = manifest.read_bytes()
    metadata = json.loads(manifest_bytes.decode('utf-8'))
    app_id = metadata.get('id', '')
    if not isinstance(app_id, str) or not re.fullmatch('[a-z0-9_]{1,7}', app_id):
        raise ValueError(f'Invalid application ID: {app_id!r}')
    if root.name != app_id or metadata.get('package_format') != 1 or metadata.get('abi') != 1:
        raise ValueError(f'{app_id}: invalid package identity or format')
    if metadata.get('build_id') != build_id:
        raise ValueError(f'{app_id}: firmware profile mismatch; rebuild this application')
    declared = set()
    if 'icon' in metadata:
        if not isinstance(metadata['icon'], str):
            raise ValueError(f'{app_id}: invalid icon path')
        declared.add(relative_name(metadata['icon']))
    libraries = metadata.get('libraries', {})
    if not isinstance(libraries, dict):
        raise ValueError(f'{app_id}: invalid library declarations')
    for name in libraries:
        if not re.fullmatch('[a-z0-9_]+', name) or name == app_id:
            raise ValueError(f'{app_id}: invalid library name: {name}')
        declared.add(relative_name(f'codecs/{name}.so'))
    index = root / 'files.sha256'
    index_bytes = index.read_bytes()
    index_digest = hashlib.sha256(index_bytes).hexdigest()
    if index_digest != metadata.get('files_sha256'):
        raise ValueError(f'{app_id}: file index digest mismatch')
    if index_bytes and not index_bytes.endswith(b'\n'):
        raise ValueError(f'{app_id}: file index requires LF-terminated lines')
    names = set()
    previous = b''
    total = len(manifest_bytes) + len(index_bytes)
    for line in index_bytes.decode('utf-8').split('\n')[:-1]:
        digest, separator, name = line.partition('  ')
        if not separator or not re.fullmatch('[0-9a-f]{64}', digest):
            raise ValueError(f'{app_id}: invalid file index line')
        relative_name(name)
        path = root / name
        if name in names or name in ('app.json', 'files.sha256'):
            raise ValueError(f'{app_id}: duplicate/reserved file: {name}')
        encoded_name = name.encode('utf-8')
        if encoded_name <= previous:
            raise ValueError(f'{app_id}: file index paths must be strictly increasing')
        previous = encoded_name
        if not path.resolve().is_relative_to(root) or is_link(path) or not path.is_file():
            raise ValueError(f'{app_id}: missing or linked file: {name}')
        if sha256(path) != digest:
            raise ValueError(f'{app_id}: payload digest mismatch: {name}')
        names.add(name)
        total += path.stat().st_size
    actual = set()
    for path in root.rglob('*'):
        if is_link(path) or not path.resolve().is_relative_to(root):
            raise ValueError(f'{app_id}: linked package entry')
        if path.is_file():
            actual.add(path.relative_to(root).as_posix())
    if actual != names | {'app.json', 'files.sha256'} or app_id + '.so' not in names:
        raise ValueError(f'{app_id}: incomplete or unindexed payload')
    if declared - names:
        raise ValueError(f'{app_id}: missing declared payload: {sorted(declared - names)}')
    return dict(id=app_id, name=metadata['name'], version=metadata['version'],
                data_version=metadata['data_version'], bytes=total,
                package=f'app-resources/{app_id}', manifest_sha256=hashlib.sha256(manifest_bytes).hexdigest(),
                files_sha256=index_digest,
                licenses=sorted(name for name in names if 'license' in name.lower() or
                                name.lower().endswith('sources.txt')))


def catalogue(firmware, names=None):
    firmware = Path(firmware).resolve()
    profile_bytes = (firmware / 'app-profile.json').read_bytes()
    profile = json.loads(profile_bytes.decode('utf-8'))
    root = firmware / 'app-resources'
    if is_link(root) or not root.resolve().is_relative_to(firmware):
        raise ValueError(f'Linked or outside release root: {root}')
    if names is None:
        names = sorted(path.name for path in root.iterdir() if path.is_dir())
    names = list(dict.fromkeys(names))
    if any(not re.fullmatch('[a-z0-9_]{1,7}', name) for name in names):
        raise ValueError('Invalid selected application ID')
    return dict(catalogue_format=1, board=profile['board'], build_id=profile['build_id'],
                profile_sha256=hashlib.sha256(profile_bytes).hexdigest(),
                applications=[inspect_package(root / name, profile['build_id'], root) for name in names])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--apps', help='Comma-separated IDs; omitted means all existing packages')
    parser.add_argument('--output', type=Path, help='Default: <firmware>/app-catalogue.json')
    args = parser.parse_args()
    names = [name.strip() for name in args.apps.split(',') if name.strip()] if args.apps else None
    result = catalogue(args.firmware, names)
    output = args.output or args.firmware / 'app-catalogue.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + '.tmp')
    temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(output)
    print(f'{len(result["applications"])} verified applications: {output}')


if __name__ == '__main__':
    main()
