"""Check native module imports against the firmware's RT-Thread symbol table."""
import subprocess
from pathlib import Path


def verify(module, firmware, nm):
    exports = set()
    for line in subprocess.check_output([str(nm), str(firmware)], text=True).splitlines():
        name = line.split()[-1] if line.split() else ''
        if name.startswith('__rtmsym_') and not name.endswith('_name'):
            exports.add(name[len('__rtmsym_'):])
    imports = [line.split()[-1] for line in subprocess.check_output(
        [str(nm), '-D', '--undefined-only', str(module)], text=True).splitlines() if line.split()]
    missing = sorted(set(imports) - exports)
    if missing:
        raise ValueError('Unexported module imports: ' + ', '.join(missing))
    print(f'{Path(module).name}: {len(imports)} imports resolved in firmware RTM table')
