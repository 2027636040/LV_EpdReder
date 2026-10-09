"""Validate locked SD package consumption through native SDK/Conan entry points."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def execute(command, cwd, env=None, success=True):
    result = subprocess.run(command, cwd=cwd, env=env, text=True, encoding="utf-8",
                            errors="replace", stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    print(result.stderr, end="")
    if success and result.returncode:
        print(result.stdout)
        raise RuntimeError(f"Failed ({result.returncode}): {command}")
    if not success and result.returncode == 0:
        raise RuntimeError("Missing-package check unexpectedly succeeded")
    return result


def main():
    repo = Path(__file__).resolve().parents[2]
    sdk = Path(os.environ["SIFLI_SDK"])
    conan = shutil.which("conan")
    if not conan:
        raise SystemExit("Activate the project's SDK environment.")
    project = repo / "project"
    lock = project / ".sf-pkg/conan.lock"
    consumer = project / ".sf-pkg/conanfile.py"
    expected = json.loads(lock.read_text(encoding="utf-8"))
    expected_refs = {value.split("%")[0] for value in expected["requires"]}
    expected_python = {value.split("%")[0] for value in expected["python_requires"]}

    def graph():
        result = execute([conan, "graph", "info", str(consumer), "--no-remote", "--format=json"], project)
        assert "Using lockfile:" in result.stderr
        nodes = json.loads(result.stdout)["graph"]["nodes"].values()
        packages = [node for node in nodes if node.get("name") == "epd-sdcard"]
        assert len(packages) == 1
        package = packages[0]
        assert package["ref"] in expected_refs
        python_refs = set(package["python_requires"])
        assert python_refs == expected_python
        return package["ref"], package["package_id"], package["prev"]

    first = graph()
    assert graph() == first
    print("Repeated offline graph:", first)
    with tempfile.TemporaryDirectory(prefix="sd-package-") as directory:
        temporary = Path(directory)
        native = temporary / "native"
        (native / ".sf-pkg").mkdir(parents=True)
        shutil.copyfile(project / "sf-pkg.yaml", native / "sf-pkg.yaml")
        shutil.copyfile(lock, native / ".sf-pkg/conan.lock")
        # Exercise the real SDK CLI with no pre-existing fingerprint/deployment.
        result = execute([sys.executable, str(sdk / "tools/sdk.py"), "sf-pkg", "install"], native)
        print(result.stdout, end="")
        assert "Using lockfile:" in result.stdout + result.stderr
        assert all(reference in result.stdout + result.stderr for reference in expected_refs)
        assert (native / "sf-pkgs/SConscript_conandeps").is_file()
        assert (native / "sf-pkgs/Kconfig.conandeps").is_file()

        offline = temporary / "offline"
        execute([conan, "install", str(consumer), "--no-remote", "--deployer=full_deploy",
                 "--envs-generation=false", f"--output-folder={offline}"], project)
        deployed = offline / "full_deploy/host/epd-sdcard/1.0.0"
        source = repo / "components/sdcard"
        compared = 0
        for file in deployed.rglob("*"):
            if not file.is_file():
                continue
            if file.parent == deployed and file.name in ("conaninfo.txt", "conanmanifest.txt"):
                continue
            original = source / file.relative_to(deployed)
            assert original.is_file(), str(file)
            assert hashlib.sha256(original.read_bytes()).digest() == hashlib.sha256(file.read_bytes()).digest(), str(file)
            compared += 1
        assert compared >= 7
        print(f"Offline deployed files match source: {compared}")

        # A fresh isolated cache must fail instead of consuming a different package.
        empty = temporary / "empty-cache"
        env = dict(os.environ, CONAN_HOME=str(empty))
        profile = execute([conan, "profile", "path", "default"], project).stdout.strip()
        (empty / "profiles").mkdir(parents=True)
        shutil.copyfile(profile, empty / "profiles/default")
        result = execute([conan, "graph", "info", str(consumer), "--no-remote"], project,
                         env=env, success=False)
        assert "epd-sdcard" in result.stderr and "not found" in result.stderr.lower()
        print("Missing dependency: rejected with no remote access; user cache unchanged")
    print("Locked native SDK install, deterministic graph, offline install and missing-package checks passed")


if __name__ == "__main__":
    main()
