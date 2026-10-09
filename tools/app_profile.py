"""Generate the firmware configuration identity used by native app packages."""
import hashlib
import json
import subprocess
from pathlib import Path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_changed(path, data):
    path = Path(path)
    if not path.exists() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def changes(root, *exclude):
    state = hashlib.sha256(git(root, 'diff', '--binary', 'HEAD', '--', '.', *exclude))
    for name in sorted(git(root, 'ls-files', '--others', '--exclude-standard', '-z').decode().split('\0')):
        if not name:
            continue
        state.update(name.encode())
        state.update((root / name).read_bytes())
    return state.hexdigest()


def generate(repo, build, config):
    repo, build = Path(repo).resolve(), Path(build).resolve()
    sdk = repo / 'SiFli-SDK'
    submodules = {}
    for line in git(sdk, 'submodule', 'status', '--recursive').decode().splitlines():
        path = line.strip().split()[1]
        root = sdk / path
        submodules[path] = {'commit': git(root, 'rev-parse', 'HEAD').decode().strip(),
                            'changes': changes(root)}
    public = [repo / 'src/ui/platform/epd_app.h', repo / 'src/ui/platform/epd_app.c',
              repo / 'src/ui/platform/app_runtime.c', repo / 'src/ui/platform/epd_input.h',
              repo / 'src/ui/platform/app_memory.h', repo / 'src/ui/platform/app_memory.c',
              repo / 'src/ui/platform/app_library.c', repo / 'src/ui/platform/app_package.h',
              repo / 'src/platform/epd_memory.h', repo / 'src/platform/epd_memory.c',
              repo / 'src/platform/epd_tls_config.h',
              repo / 'src/platform/epd_image.h', repo / 'src/platform/epd_image.c',
              repo / 'src/ui/ui_settings.h',
              repo / 'src/services/storage.h', repo / 'src/services/storage_file.h',
              repo / 'src/ui/platform/app_image.h', repo / 'src/services/app_service.h',
              repo / 'src/services/net/network.h']
    compiler = Path(config.EXEC_PATH) / config.CC
    profile = {
        'profile_format': 1,
        'board': build.name.removeprefix('build_'),
        'sdk_commit': git(sdk, 'rev-parse', 'HEAD').decode().strip(),
        # SDK regenerates certificate order from a Python set on each invocation.
        # Certificate data does not change the native app ABI.
        'sdk_changes': changes(sdk, ':(exclude)external/mbedtls_228/ports/src/tls_certificate.c'),
        'sdk_submodules': submodules,
        'compiler': subprocess.check_output([str(compiler), '--version']).decode().splitlines()[0],
        'module_cflags': config.M_CFLAGS,
        'module_ldflags': config.M_LFLAGS,
        'configuration': {name: digest((build / name).read_bytes())
                          for name in ('rtconfig.h', 'cconfig.h')},
        'platform_interface': {path.name: digest(path.read_bytes()) for path in public},
    }
    profile['build_id'] = digest(json.dumps(profile, sort_keys=True, ensure_ascii=True).encode())
    write_changed(build / 'app-profile.json',
                  (json.dumps(profile, ensure_ascii=False, indent=2) + '\n').encode())
    write_changed(build / 'epd_app_profile.h',
                  ('#ifndef EPD_APP_PROFILE_H\n#define EPD_APP_PROFILE_H\n'
                   f'#define EPD_APP_BUILD_ID "{profile["build_id"]}"\n#endif\n').encode())
