"""Build the NOR image using the matching SDK's mklfsimg executable."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import tempfile


def flash_region(ptab, name):
    for memory in json.loads(Path(ptab).read_text(encoding='utf-8')):
        for region in memory.get('regions', []):
            if region.get('name') == name:
                return int(memory['base'], 0) + int(region['offset'], 0), int(region['max_size'], 0)
    raise ValueError('Missing partition: ' + name)


def verify_import(directory):
    directory = Path(directory).resolve()
    manifest = json.loads((directory / 'migration.json').read_text(encoding='utf-8'))
    if manifest['format'] != 1:
        raise ValueError('Unsupported storage migration format')
    actual = {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (directory / 'flash').rglob('*') if p.is_file()}
    actual['ble.bin'] = hashlib.sha256((directory / 'ble.bin').read_bytes()).hexdigest()
    if actual != manifest['files'] or (directory / 'ble.bin').stat().st_size != 0x4000:
        raise ValueError('Storage backup file list/checksum mismatch')
    return directory / 'flash'


def tool_sources(sdk):
    sdk = Path(sdk).resolve()
    return [sdk / 'tools/mklfsimg/mklfsimg/mklfs.c', sdk / 'tools/mklfsimg/mklfsimg/getopt.c',
            sdk / 'rtos/rtthread/components/dfs/filesystems/littlefs/lfs.c',
            sdk / 'rtos/rtthread/components/dfs/filesystems/littlefs/lfs_util.c']


def build_tool(target, sdk):
    """Use the SDK sources unchanged; a process manifest makes Win32 ANSI APIs UTF-8."""
    target = Path(target).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    vs = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                  'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                  '-property', 'installationPath'], text=True).strip()
    if not vs:
        raise RuntimeError('Visual Studio C++ build tools are required for the SDK image tool')
    vcvars = Path(vs) / 'VC/Auxiliary/Build/vcvars64.bat'
    output = subprocess.check_output(f'"{vcvars}" >nul && set', shell=True,
                                      text=True, errors='replace')
    environment = dict(line.split('=', 1) for line in output.splitlines() if '=' in line and not line.startswith('='))
    compiler = Path(environment['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    lfs = Path(sdk).resolve() / 'rtos/rtthread/components/dfs/filesystems/littlefs'
    subprocess.run([str(compiler), '/nologo', '/O2', '/std:c11', '/utf-8',
                    '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(lfs),
                    *map(str, tool_sources(sdk)), '/Fe:' + str(target), '/link', '/MANIFEST:EMBED',
                    '/MANIFESTINPUT:' + str(Path(__file__).with_name('mklfsimg.manifest'))],
                   cwd=target.parent, env=environment, check=True)


def build_image(directory, target, tool, ptab):
    directory, target = Path(directory).resolve(), Path(target).resolve()
    _, size = flash_region(ptab, 'fs_root')
    # This SDK's Windows tool uses the first '/' as the image-relative path.
    # Pass a backslash-only absolute source directory (never a POSIX/relative path).
    if os.name != 'nt':
        raise RuntimeError('The current SDK image tool requires Windows')
    if not directory.is_dir():
        raise ValueError('Missing image source directory: ' + str(directory))
    for file in directory.rglob('*'):
        if file.is_symlink():
            raise ValueError('Image source must not contain symlinks: ' + str(file))
        if len(str(file).encode('utf-8')) >= 256:
            raise ValueError('SDK image tool path exceeds 255 bytes: ' + str(file))
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(tool), '-c', str(directory), '-b', '4096', '-r', '32', '-p', '256',
                    '-s', str(size), '-i', str(target)], check=True)
    if target.stat().st_size != size or b'littlefs' not in target.read_bytes()[:8192]:
        raise ValueError('Invalid LittleFS image output')


def add_image(env, sdk, migration=None, packages=()):
    from SCons.Script import Action, Value
    directory = verify_import(migration) if migration else Path('../disk').resolve()
    ptab = env['PARTITION_TABLE']
    target = str(Path(env['build_dir']) / 'fs_root.bin')
    tool = Path(env['build_dir']) / 'host/mklfsimg.exe'

    def compile_tool(target, source, env):
        build_tool(str(target[0]), sdk)
        return 0

    lfs = Path(sdk) / 'rtos/rtthread/components/dfs/filesystems/littlefs'
    tool_node = env.Command(str(tool), [*map(str, tool_sources(sdk)), str(lfs / 'lfs.h'),
                            str(lfs / 'lfs_util.h'), str(Path(__file__).with_name('mklfsimg.manifest')),
                            __file__], compile_tool)
    files = sorted(p for p in directory.rglob('*') if p.is_file())

    def action(target, source, env):
        if migration:
            verify_import(migration)
        # Overlay built-in app resources without changing disk/ or the old backup.
        with tempfile.TemporaryDirectory(prefix='lfs-', dir=tool.parent) as staging:
            staging = Path(staging)
            shutil.copytree(directory, staging, dirs_exist_ok=True)
            for package, nodes in packages:
                destination = staging / 'apps' / package.name
                if not destination.exists():
                    shutil.copytree(package, destination)
            build_image(staging, str(target[0]), tool, ptab)
        return 0

    dependencies = [str(p) for p in files] + [str(ptab), __file__,
                    tool_node,
                    Value(str(directory) + '\n' + '\n'.join(str(p) for p in files))]
    if migration:
        dependencies += [str(Path(migration) / 'migration.json'), str(Path(migration) / 'ble.bin')]
    for package, nodes in packages:
        dependencies += nodes
    nodes = env.Command(target, dependencies, Action(action, 'LittleFS $TARGET'))
    env.Depends(env['target'], nodes)
    return target
