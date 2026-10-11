"""Exercise production WeRead page callbacks with real LVGL on Linux/WSL.

The framework substitute follows the local SDK contract: callback parameter is
app_id, user_data is borrowed, and page memory is separate zeroed storage.
Network, reader processing and platform widgets/services are substitutes.
"""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="weread-ui-") as directory:
        build = Path(directory)
        for command in (
            ["cmake", "-S", str(here / "ui"), "-B", str(build)],
            ["cmake", "--build", str(build), "-j", "8"],
        ):
            result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                print(result.stdout)
                result.check_returncode()
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:detect_stack_use_after_return=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(build / "ui_test")], env=env, check=True, timeout=60)


if __name__ == "__main__":
    main()
