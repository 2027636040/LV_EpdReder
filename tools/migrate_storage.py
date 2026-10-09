"""Export the pre-LittleFS 16 MiB NOR readback without writing to a device.

The legacy binary layouts below correspond to weather_store v1, settings_store
v1 and ui_bookshelf_data history EPR1 in this project (32-bit little endian).
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import zlib


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def write_new(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as stream:
        stream.write(data)


def record(magic, payload):
    return struct.pack('<IIII', magic, 1, len(payload), zlib.crc32(payload)) + payload


def latest(data, offset, slots, size, magic):
    selected = None
    for index in range(slots):
        value = data[offset + index * 4096:offset + index * 4096 + size]
        tag, crc, version, sequence = struct.unpack_from('<IIII', value)
        if tag != magic or version != 1 or crc != zlib.crc32(value[8:]):
            continue
        if selected is None or 0 < ((sequence - struct.unpack_from('<I', selected, 12)[0]) & 0xffffffff) < 0x80000000:
            selected = value
    return selected


def safe_relative(name):
    path = PurePosixPath(name.lstrip('/'))
    if not path.parts or any(p in ('.', '..') or '\\' in p or ':' in p for p in path.parts):
        raise ValueError('Unsafe FAT path: ' + name)
    return path


def migrate_history(root, report):
    count = 0
    directory = root / '.epd_reader'
    if not directory.is_dir():
        return
    for file in sorted(directory.iterdir()):
        if file.suffix not in ('.a', '.b') or not file.is_file():
            continue
        raw = file.read_bytes()
        if len(raw) != 300 or struct.unpack_from('<I', raw)[0] != 0x45505231 or fnv(raw[:296]) != struct.unpack_from('<I', raw, 296)[0]:
            report['ignored_history'] += 1
            continue
        name = raw[16:272].split(b'\0', 1)[0].decode('utf-8')
        relative = name[6:] if name.startswith('/flash/') else name
        book = root.joinpath(*safe_relative(relative).parts)
        if not book.is_file() or book.stat().st_size != struct.unpack_from('<I', raw, 8)[0]:
            report['ignored_history'] += 1
            continue
        modified = fnv(book.read_bytes())
        data = bytearray(raw)
        struct.pack_into('<I', data, 12, modified)
        struct.pack_into('<I', data, 296, fnv(data[:296]))
        key = fnv(relative.encode('utf-8'))
        write_new(root / 'data/reader' / f'flash-{key:08x}{file.suffix}', data)
        count += 1
    report['reading_records'] = count
    # Keep the original histories/indexes outside the new image for recovery.
    directory.rename(root.parent / 'legacy-reader')


def migrate(dump, output):
    from pyfatfs.PyFatFS import PyFatFS
    dump = dump.resolve()
    if dump.stat().st_size != 0x1000000:
        raise ValueError('Input must be the complete 16 MiB NOR readback starting at 0x12000000')
    data = dump.read_bytes()
    # Reject a different layout instead of interpreting its former data addresses.
    fat = data[0xe20000:0xee0000]
    if fat[510:512] != b'\x55\xaa' or struct.unpack_from('<H', fat, 11)[0] not in (512, 1024, 2048, 4096):
        raise ValueError('Legacy FAT filesystem not found at offset 0xE20000; confirm the old layout')
    output.mkdir(parents=True, exist_ok=False)
    root = output / 'flash'
    root.mkdir()
    write_new(output / 'legacy-fat.bin', fat)
    write_new(output / 'ble.bin', data[0x1a4000:0x1a8000])
    report = {'format': 1, 'source_sha256': hashlib.sha256(data).hexdigest(),
              'weather': False, 'settings': False, 'reading_records': 0, 'ignored_history': 0}
    with PyFatFS(str(output / 'legacy-fat.bin'), encoding='cp936', read_only=True) as fs:
        def copy_dir(source):
            for name in fs.listdir(source):
                if name in ('.', '..'):
                    continue
                path = source.rstrip('/') + '/' + name
                target = root.joinpath(*safe_relative(path).parts)
                if fs.isdir(path):
                    target.mkdir()
                    copy_dir(path)
                else:
                    with fs.openbin(path, 'r') as stream:
                        write_new(target, stream.read())
        copy_dir('/')
    # Firmware uses -fshort-enums: weather_info_t is 716 bytes, not 720.
    weather = latest(data, 0x1b0000, 16, 1124, 0x57583031)
    if weather:
        config, info = weather[16:408], weather[408:1124]
        for start, length in ((0, 128), (128, 128), (256, 16), (272, 48), (320, 64)):
            if b'\0' not in config[start:start + length]:
                raise ValueError('Invalid legacy weather string')
        write_new(root / 'data/weather/config.bin', record(0x57584332, config))
        write_new(root / 'cache/weather/current.bin', record(0x57584932, struct.pack('<I', zlib.crc32(config)) + info))
        report['weather'] = True
    settings = latest(data, 0x1c0000, 2, 32, 0x45505331)
    if settings:
        values = settings[16:29]
        if any(v >= limit for v, limit in zip(values, (2, 5, 6, 2, 2, 2, 2, 5, 8, 3, 5, 5, 3))):
            raise ValueError('Invalid legacy settings values')
        write_new(root / 'system/settings.bin', record(0x45505332, values))
        report['settings'] = True
    migrate_history(root, report)
    for name in ('apps', 'data', 'cache', 'system', 'fonts', 'update'):
        (root / name).mkdir(exist_ok=True)
    report['files'] = {p.relative_to(output).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                       for p in root.rglob('*') if p.is_file()}
    report['files']['ble.bin'] = hashlib.sha256((output / 'ble.bin').read_bytes()).hexdigest()
    write_new(output / 'migration.json', json.dumps(report, ensure_ascii=False, indent=2).encode('utf-8'))
    print(f'Exported {len(report["files"])} files; weather={report["weather"]}, settings={report["settings"]}, '
          f'reading_records={report["reading_records"]}, ignored_history={report["ignored_history"]}')
    print('The source NOR readback is unchanged. No device was written.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='New, non-existent backup directory')
    args = parser.parse_args()
    migrate(args.dump, args.out.resolve())


if __name__ == '__main__':
    main()
