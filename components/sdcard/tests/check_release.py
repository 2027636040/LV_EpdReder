"""Read-only SD package metadata and deployed-runtime checks (no Conan writes)."""

import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import urlsplit


PACKAGE = Path(__file__).resolve().parents[1]
RUNTIME_FILES = ("Kconfig", "SConscript", "include/sdcard.h", "src/sdcard.c",
                 "port/sdcard_port.h", "port/sdcard_sifli_sdio.c")
EXPORTS = ("conandata.yml", "Kconfig", "SConscript", "LICENSE", "README.md",
           "include/*", "src/*", "port/*", "example/*", "tests/*")
PLACEHOLDERS = {"publisher", "namespace", "yourname", "your-namespace", "todo", "tbd"}


def read_recipe(path):
    """Read literal class metadata without importing or executing the recipe."""
    tree = ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
    recipes = [node for node in tree.body if isinstance(node, ast.ClassDef)
               and any(isinstance(base, ast.Name) and base.id == "ConanFile"
                       for base in node.bases)]
    if len(recipes) != 1:
        raise ValueError("Expected one ConanFile recipe class")
    fields = {}
    for node in recipes[0].body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    fields[target.id] = ast.literal_eval(node.value)
    return fields


def valid_namespace(value):
    return (isinstance(value, str) and value not in PLACEHOLDERS
            and re.fullmatch(r"[a-z0-9][a-z0-9_.-]*", value) is not None)


def valid_url(value):
    if not isinstance(value, str) or re.search(r"[\s<>]", value):
        return False
    try:
        url = urlsplit(value)
        host = url.hostname or ""
        url.port  # Reject malformed port numbers as well as malformed hosts.
    except ValueError:
        return False
    reserved = ("example.com", "example.org", "example.net", "invalid", "test", "localhost")
    return (url.scheme in ("http", "https") and "." in host
            and not url.username and not url.password and not url.fragment
            and not any(host == item or host.endswith("." + item) for item in reserved))


def compare_deployed(package, deployed):
    """Compare runtime inputs and existing packaged material, without repackaging."""
    if not deployed.is_dir() or not (deployed / "conanmanifest.txt").is_file():
        raise ValueError("Deployed package must contain Conan's conanmanifest.txt")

    def matches(relative):
        local, installed = package / relative, deployed / relative
        return (local.is_file() and installed.is_file()
                and hashlib.sha256(local.read_bytes()).digest()
                == hashlib.sha256(installed.read_bytes()).digest())

    runtime = set(RUNTIME_FILES)
    for folder in ("include", "src", "port"):
        for root in (package, deployed):
            runtime.update(path.relative_to(root).as_posix()
                           for path in (root / folder).rglob("*") if path.is_file()
                           and path.suffix.lower() not in (".o", ".obj"))
    mismatches = sorted(name for name in runtime if not matches(name))
    material = {"LICENSE", "README.md", "conandata.yml"}
    for folder in ("example", "tests"):
        for root in (package, deployed):
            material.update(path.relative_to(root).as_posix()
                            for path in (root / folder).rglob("*") if path.is_file()
                            and "__pycache__" not in path.parts and path.suffix != ".pyc")
    return {"runtime_files_checked": len(runtime), "runtime_mismatches": mismatches,
            "material_differences": sorted(name for name in material if not matches(name))}


def check(package=PACKAGE, namespace=None, deployed=None):
    package = Path(package)
    report = {"local_errors": [], "release_errors": [], "deployed": None,
              "publication_verified": False}
    errors, release = report["local_errors"], report["release_errors"]
    try:
        fields = read_recipe(package / "conanfile.py")
        for name in ("name", "version", "license", "description", "support_sdk_version"):
            if not isinstance(fields.get(name), str) or not fields[name].strip():
                errors.append(f"Missing non-empty recipe field: {name}")
        for name, expected in (("name", "epd-sdcard"),
                               ("python_requires", "sf-pkg-base/[^1.0]@sifli"),
                               ("python_requires_extend", "sf-pkg-base.SourceOnlyBase")):
            if fields.get(name) != expected:
                errors.append(f"{name} must be {expected}")
        exports = fields.get("exports_sources", ())
        if not isinstance(exports, (tuple, list)) or not all(isinstance(x, str) for x in exports):
            errors.append("exports_sources must be a literal sequence of patterns")
        else:
            for pattern in EXPORTS:
                if pattern not in exports:
                    errors.append(f"Missing export pattern: {pattern}")
        for name in (*RUNTIME_FILES, "LICENSE", "README.md", "conandata.yml",
                     "example/consumer/sf-pkg.yaml"):
            if not (package / name).is_file() or not (package / name).stat().st_size:
                errors.append(f"Missing or empty package file: {name}")
        user = fields.get("user")
        if not valid_namespace(user):
            release.append("user: supply the actual registered publisher namespace")
        if namespace is not None and (not valid_namespace(namespace) or user != namespace):
            release.append("Selected namespace does not match recipe user")
        for name in ("url", "homepage"):
            if not valid_url(fields.get(name)):
                release.append(f"{name}: supply an actual HTTP(S) URL, not a placeholder")
        report["reference"] = f"{fields.get('name', '')}/{fields.get('version', '')}"
        if valid_namespace(user):
            report["reference"] += "@" + user
    except (OSError, SyntaxError, ValueError, TypeError) as error:
        errors.append(f"Recipe cannot be checked: {error}")
    try:
        lock = json.loads((package / "conan.lock").read_text(encoding="utf-8-sig"))
        refs = lock.get("python_requires", [])
        if lock.get("version") != "0.5" or not isinstance(refs, list) or len(refs) != 1:
            raise ValueError("Expected a Conan 0.5 lock with one sf-pkg-base python requirement")
        if not isinstance(refs[0], str) or not re.fullmatch(
                r"sf-pkg-base/1\.\d+\.\d+@sifli#[0-9a-f]{32}(?:%\d+(?:\.\d+)?)?", refs[0]):
            raise ValueError("sf-pkg-base must be locked to a concrete 1.x version and recipe revision")
        report["locked_python_require"] = refs[0].split("%")[0]
    except (OSError, ValueError, AttributeError, TypeError) as error:
        errors.append(f"Dependency lock cannot be checked: {error}")
    if deployed is not None:
        try:
            report["deployed"] = compare_deployed(package, Path(deployed))
            if report["deployed"]["runtime_mismatches"]:
                errors.append("Runtime inputs differ from the deployed package; review its pinned revision")
        except (OSError, ValueError) as error:
            errors.append(f"Deployed package cannot be checked: {error}")
    report["local_ready"] = not errors
    report["release_metadata_ready"] = not errors and not release
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path, default=PACKAGE)
    parser.add_argument("--namespace", help="Expected publisher; does not fill in recipe metadata")
    parser.add_argument("--deployed-package", type=Path, help="Existing Conan package deployment to compare")
    parser.add_argument("--strict-release", action="store_true", help="Fail on missing release metadata")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    args = parser.parse_args(argv)
    report = check(args.package_root, args.namespace, args.deployed_package)
    if args.format == "json":
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        print("Local metadata:", "PASS" if report["local_ready"] else "FAIL")
        print("Release metadata:", "PASS" if report["release_metadata_ready"] else "INCOMPLETE")
        for key in ("local_errors", "release_errors"):
            for error in report[key]:
                print(f"  {key}: {error}")
        if report["deployed"] is not None:
            print("Deployed comparison:", json.dumps(report["deployed"], ensure_ascii=False))
        print("Publication is not verified. No remote access, package creation or upload was performed.")
    return int(not report["local_ready"] or
               (args.strict_release and not report["release_metadata_ready"]))


if __name__ == "__main__":
    raise SystemExit(main())
