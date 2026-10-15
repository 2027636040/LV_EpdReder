"""Prepare the pinned SDK patches and local SD package before SCons configuration."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


REPO = Path(__file__).resolve().parents[1]


def run(command, cwd, **kwargs):
    return subprocess.run(command, cwd=cwd, check=True, **kwargs)


def prepare_sdk():
    sdk = REPO / "SiFli-SDK"
    manifest_path = REPO / "patches/SiFli-SDK/series.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    revision = run(["git", "rev-parse", "HEAD"], sdk,
                   capture_output=True, text=True).stdout.strip()
    if revision != manifest["base"]:
        raise RuntimeError(
            f"SDK HEAD is {revision}, expected {manifest['base']}. "
            "Preserve any SDK edits, then run git submodule update --init --recursive "
            "from the repository root; do not use submodule update --remote.")
    pending = []
    # Check the entire series before changing files. Already applied patches
    # are accepted; conflicts leave the developer's working tree untouched.
    for entry in manifest["patches"]:
        patch = manifest_path.parent / entry["file"]
        command = ["git", "apply", "--ignore-space-change", "--check"]
        if subprocess.run(command + ["--reverse", str(patch)], cwd=sdk,
                          capture_output=True).returncode == 0:
            print(f"SDK: already applied {patch.name}")
            continue
        run(command + [str(patch)], sdk)
        pending.append(patch)
    for patch in pending:
        run(["git", "apply", "--ignore-space-change", str(patch)], sdk)
        print(f"SDK: applied {patch.name}")


def prepare_packages():
    sdk = REPO / "SiFli-SDK"
    configured_sdk = os.environ.get("SIFLI_SDK")
    if not configured_sdk or Path(configured_sdk).resolve() != sdk.resolve():
        raise RuntimeError("Activate this repository's SiFli-SDK/export.ps1 first.")
    component = REPO / "components/sdcard"
    lock = json.loads((component / "conan.lock").read_text(encoding="utf-8"))
    for reference in lock["python_requires"]:
        reference = reference.split("%", 1)[0]
        present = subprocess.run(
            ["conan", "cache", "path", reference], cwd=component,
            capture_output=True).returncode == 0
        if not present:
            run(["conan", "download", reference, "--only-recipe", "-r=artifactory"], component)
    run(["conan", "create", ".", "--version=1.0.0", "--no-remote",
         "--lockfile=conan.lock"], component)
    # Do not pass --board: the package Kconfig must exist before resolving it.
    run([sys.executable, str(sdk / "tools/sdk.py"), "sf-pkg", "install"], REPO / "project")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-only", action="store_true", help="Apply SDK patches without installing packages")
    args = parser.parse_args()
    prepare_sdk()
    if not args.sdk_only:
        prepare_packages()
    print("Build dependencies prepared.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
