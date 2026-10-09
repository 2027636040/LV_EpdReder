"""Build a directory package without executing its native module."""
import argparse
import hashlib
import json
import re
import shutil
import struct
from pathlib import Path


def sha256(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4096), b''):
            result.update(block)
    return result.hexdigest()


def elf_load_bytes(data):
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Module must be a 32-bit little-endian ELF')
    fields = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    kind, machine, phoff, phsize, phcount = fields[0], fields[1], fields[4], fields[8], fields[9]
    if kind != 3 or machine != 40 or phsize != 32 or phoff + phcount * phsize > len(data):
        raise ValueError('Module must be an ARM ET_DYN image with valid program headers')
    start = end = None
    for i in range(phcount):
        ptype, offset, vaddr, _, filesz, memsz, _, _ = struct.unpack_from('<8I', data, phoff + i * phsize)
        if ptype != 1:
            continue
        if filesz > memsz or offset + filesz > len(data) or vaddr + memsz > 0xffffffff:
            raise ValueError('Invalid ELF load segment')
        if end is not None and vaddr < end:
            raise ValueError('ELF load segments must be sorted and not overlap')
        if start is None:
            start = vaddr
        end = vaddr + memsz
    if start is None or end <= start:
        raise ValueError('Module has no loadable bytes')
    return end - start


def relative_name(name):
    parts = name.split('/')
    if (not name or len(name.encode()) >= 160 or len(parts) > 8 or
            any(part in ('', '.', '..') for part in parts) or
            any(ord(char) < 32 or char in '\\:' for char in name)):
        raise ValueError(f'Unsupported package path: {name!r}')
    return name


def build_package(manifest_path, module_path, profile_path, destination, resources=None, icon=None,
                  libraries=None):
    metadata = json.loads(Path(manifest_path).read_text(encoding='utf-8'))
    app_id = metadata.get('id', '')
    if not isinstance(app_id, str) or not re.fullmatch('[a-z0-9_]{1,7}', app_id):
        raise ValueError('Application id must contain 1-7 lowercase letters, digits or underscores')
    if not isinstance(metadata.get('name'), str) or not 0 < len(metadata['name'].encode()) < 48:
        raise ValueError('Application name must occupy 1-47 UTF-8 bytes')
    for field in ('version', 'data_version'):
        if type(metadata.get(field)) is not int or not 0 < metadata[field] <= 0xffffffff:
            raise ValueError(f'{field} must be a positive uint32')
    if metadata.get('abi') != 1 or type(metadata.get('background', False)) is not bool:
        raise ValueError('ABI must be 1 and background must be boolean')
    if type(metadata.get('settings', False)) is not bool:
        raise ValueError('settings must be boolean')
    profile = json.loads(Path(profile_path).read_text(encoding='utf-8'))
    data = Path(module_path).read_bytes()
    marker = ('EPDAPP:' + profile['build_id']).encode() + b'\0'
    if marker not in data:
        raise ValueError('Module was not rebuilt against this firmware app profile')
    load_bytes = elf_load_bytes(data)
    files = {app_id + '.so': Path(module_path)}
    library_spans = {}
    library_images = {}
    for library in libraries or ():
        library = Path(library)
        name = library.stem
        if (library.suffix != '.so' or not re.fullmatch('[a-z0-9_]{1,7}', name) or
                name == app_id or name in library_spans):
            raise ValueError('Invalid or duplicate private library: ' + str(library))
        image = library.read_bytes()
        if marker not in image:
            raise ValueError('Private library firmware profile mismatch: ' + str(library))
        library_spans[name] = elf_load_bytes(image)
        library_images[name] = image
        files['codecs/' + library.name] = library
    if resources:
        resource_root = Path(resources).resolve()
        if not resource_root.is_dir():
            raise ValueError('Resource directory does not exist')
        for path in sorted(resource_root.rglob('*')):
            if path.is_symlink() or not path.resolve().is_relative_to(resource_root):
                raise ValueError('Package resources cannot contain links')
            if path.is_file():
                files[relative_name('res/' + path.relative_to(resource_root).as_posix())] = path
    if icon:
        files['icon.ezip'] = Path(icon)
    # Each invocation creates a new generation; older packages are never recursively removed.
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=False)
    lines = []
    for name, source in sorted(files.items()):
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        if name == app_id + '.so' and target.read_bytes() != data:
            raise ValueError('Module changed while packaging; rebuild the package')
        if name.startswith('codecs/') and target.read_bytes() != library_images[Path(name).stem]:
            raise ValueError('Private library changed while packaging; rebuild the package')
        lines.append(sha256(target) + '  ' + name + '\n')
    index = destination / 'files.sha256'
    index.write_text(''.join(lines), encoding='utf-8', newline='\n')
    package = {key: metadata[key] for key in ('abi', 'id', 'version', 'name', 'data_version')}
    package.update(package_format=1, build_id=profile['build_id'], files_sha256=sha256(index),
                   settings=metadata.get('settings', False), background=metadata.get('background', False), load_bytes=load_bytes)
    if icon:
        package['icon'] = 'icon.ezip'
    if library_spans:
        package['libraries'] = library_spans
    encoded = (json.dumps(package, ensure_ascii=False, indent=2) + '\n').encode()
    (destination / 'app.json').write_bytes(encoded)
    return {'id': app_id, 'files': len(files), 'module_bytes': len(data), 'load_bytes': load_bytes,
            'minimum_load_peak': len(data) + load_bytes,
            'libraries': {name: {'file_bytes': len(library_images[name]), 'load_bytes': span,
                                 'minimum_load_peak': len(library_images[name]) + span}
                          for name, span in library_spans.items()},
            'package': str(destination)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--module', required=True)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--resources')
    parser.add_argument('--icon')
    parser.add_argument('--library', action='append', default=[])
    args = parser.parse_args()
    try:
        print(json.dumps(build_package(args.manifest, args.module, args.profile, args.output,
                                       args.resources, args.icon, args.library), ensure_ascii=False, indent=2))
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, f'Package failed: {error}\n')
