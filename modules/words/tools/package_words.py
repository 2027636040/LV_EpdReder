"""Package words.so, its independent icon and offline dictionary into app-resources/words."""
import argparse
import json
from pathlib import Path
import shutil
import sys
import tempfile

MODULE = Path(__file__).resolve().parents[1]
REPO = MODULE.parents[1]
sys.path.insert(0, str(REPO / 'tools'))
from package_app import build_package
from check_app_imports import verify


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, default=REPO / 'project/build_dpi-hdk_lb57gyd7n6_epd_hcpu')
    parser.add_argument('--dictionary', type=Path, default=MODULE / 'output/dictionaries/unified-full/library.wdb')
    parser.add_argument('--icon', type=Path)
    parser.add_argument('--output', type=Path, help='New archive directory; defaults to firmware/app-resources/words')
    args = parser.parse_args()
    icon = args.icon or args.firmware / 'words-images/icon.ezip'
    destination = args.output or args.firmware / 'app-resources/words'
    try:
        verify(MODULE / 'output/words.so', args.firmware / 'main.elf', 'arm-none-eabi-nm')
        with tempfile.TemporaryDirectory(prefix='words-resources-') as temporary:
            resources = Path(temporary) / 'res'
            resources.mkdir()
            for name in ('LICENSE.fsrs', 'LICENSE.ecdict', 'SOURCES.txt'):
                shutil.copyfile(MODULE / name, resources / name)
            shutil.copyfile(args.dictionary, resources / 'library.wdb')
            shutil.copyfile(args.dictionary.with_suffix('.wdb.json'), resources / 'library.wdb.json')
            staged = Path(temporary) / 'package'
            result = build_package(MODULE / 'app.json', MODULE / 'output/words.so',
                                   args.firmware / 'app-profile.json', staged, resources, icon)
            shutil.copytree(staged, destination, dirs_exist_ok=args.output is None)
            result['package'] = str(destination.resolve())
        print(json.dumps(result, indent=2, ensure_ascii=False))
    except (OSError, ValueError) as error:
        parser.exit(1, f'Package failed: {error}\n')
