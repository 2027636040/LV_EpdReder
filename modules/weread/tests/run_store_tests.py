"""Run the real WeRead store with host filesystem/allocation fault injection."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="weread-store-") as temporary:
    executable = Path(temporary) / "store_test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE",
                    "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I" + str(root / "include"), "-I" + str(root.parents[2] / "src/thirdparty/miniz"),
                    str(root / "store_test.c"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], cwd=temporary, check=True)
