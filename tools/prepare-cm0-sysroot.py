#!/usr/bin/env python3
"""Extract the locked ARM64 development packages; never execute target code."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import tempfile


ENVIRONMENT = dict(os.environ, LC_ALL="C")
REQUIRED_FILES = (
    "usr/include/libcamera/libcamera/camera.h",
    "usr/lib/aarch64-linux-gnu/libcamera.so",
    "usr/lib/aarch64-linux-gnu/libcamera-base.so",
    "usr/lib/aarch64-linux-gnu/libfreetype.so",
    "usr/lib/aarch64-linux-gnu/libdrm.so",
    "var/lib/dpkg/status",
    "var/lib/dpkg/info/format",
)


def checksum(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_archive(package: dict, cache: Path) -> Path:
    archive = cache / f"{package['SHA256']}.deb"
    if archive.is_file() and checksum(archive) == package["SHA256"]:
        return archive
    partial = archive.with_suffix(".partial")
    try:
        print(f"Downloading {package['Package']} {package['Version']}", flush=True)
        subprocess.run(
            ["curl", "--fail", "--location", "--silent", "--show-error",
             "--retry", "3", "--connect-timeout", "30", "--max-time", "300",
             "--output", str(partial), package["URL"]], check=True)
        if checksum(partial) != package["SHA256"]:
            raise ValueError(f"SHA-256 mismatch for {package['Package']}")
        partial.replace(archive)
    finally:
        partial.unlink(missing_ok=True)
    return archive


def extract_package(archive: Path, package: dict, root: Path) -> tuple[str, list[str]]:
    contents = subprocess.check_output(
        ["dpkg-deb", "--ctrl-tarfile", str(archive)], env=ENVIRONMENT)
    with tarfile.open(fileobj=io.BytesIO(contents)) as stream:
        controls = {entry.name.removeprefix("./"): stream.extractfile(entry).read()
                    for entry in stream if entry.isfile() and
                    entry.name.removeprefix("./") in ("control", "symbols", "shlibs")}
    control = controls["control"].decode()
    fields = dict(line.split(": ", 1) for line in control.splitlines()
                  if ": " in line and not line.startswith((" ", "\t")))
    if any(fields.get(key) != package[key] for key in ("Package", "Version", "Architecture")):
        raise ValueError(f"Package identity does not match lock: {archive}")
    if fields["Architecture"] not in ("arm64", "all"):
        raise ValueError(f"Not an ARM64 target package: {archive}")

    paths = []
    with subprocess.Popen(["dpkg-deb", "--fsys-tarfile", str(archive)],
                          stdout=subprocess.PIPE, env=ENVIRONMENT) as process:
        with tarfile.open(fileobj=process.stdout, mode="r|") as stream:
            for entry in stream:
                path = PurePosixPath(entry.name)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError(f"Unsafe package path: {entry.name}")
                paths.append("/" + (path.as_posix() if path.parts else "."))
        process.stdout.read()
        if process.wait() != 0:
            raise ValueError(f"Cannot list package files: {archive}")
    subprocess.run(["dpkg-deb", "--extract", str(archive), str(root)],
                   check=True, env=ENVIRONMENT)

    # Retain the real symbols/shlibs and ownership data for dpkg-shlibdeps.
    # A manifest of package names alone cannot provide symbol-level bounds.
    info = root / "var/lib/dpkg/info"
    info.mkdir(parents=True, exist_ok=True)
    package_id = fields["Package"]
    if fields.get("Multi-Arch") == "same":
        package_id += ":" + fields["Architecture"]
    written = []
    (info / f"{package_id}.list").write_text("\n".join(paths) + "\n")
    written.append(f"var/lib/dpkg/info/{package_id}.list")
    for name in ("symbols", "shlibs"):
        if name in controls:
            target = info / f"{package_id}.{name}"
            target.write_bytes(controls[name])
            written.append(target.relative_to(root).as_posix())
    return control.rstrip() + "\nStatus: install ok installed\n", written


def make_symlinks_relative(root: Path) -> None:
    for directory, dirs, files in os.walk(root):
        for name in dirs + files:
            path = Path(directory) / name
            if path.is_symlink() and os.readlink(path).startswith("/"):
                target = root / os.readlink(path).lstrip("/")
                path.unlink()
                path.symlink_to(os.path.relpath(target, path.parent))


def ready(root: Path, identity: dict) -> bool:
    try:
        state = json.loads((root / ".camera-sysroot.json").read_text())
        return (state["identity"] == identity and
                all((root / path).is_file() for path in (*REQUIRED_FILES, *state["metadata"])))
    except (OSError, ValueError, KeyError, TypeError):
        return False


def prepare(lock_file: Path, output: Path) -> None:
    lock_file, output = lock_file.resolve(), output.resolve()
    identity = {"lock": checksum(lock_file), "preparer": checksum(Path(__file__))}
    cache = output.parent
    cache.mkdir(parents=True, exist_ok=True)
    with (cache / ".prepare.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if ready(output, identity):
            print(f"Using cached CM0 camera sysroot: {output}")
            return
        if output.exists() and not (output / ".camera-sysroot.json").is_file():
            raise ValueError(f"Refusing to replace an unmanaged sysroot: {output}")
        for tool in ("curl", "dpkg-deb"):
            if not shutil.which(tool):
                raise ValueError(f"Missing {tool}; on macOS install with: brew install curl dpkg")
        manifest = json.loads(lock_file.read_text())
        if manifest.get("schema_version") != 1 or manifest.get("architecture") != "arm64":
            raise ValueError("Expected a version 1 ARM64 sysroot lock")
        packages = manifest["packages"]
        archives = cache / "archives"
        archives.mkdir(exist_ok=True)
        with ThreadPoolExecutor(max_workers=4) as executor:
            downloads = list(executor.map(lambda p: fetch_archive(p, archives), packages))

        with tempfile.TemporaryDirectory(prefix="preparing-", dir=cache) as temporary:
            staging = Path(temporary) / "sysroot"
            staging.mkdir()
            (staging / "usr/lib").mkdir(parents=True)
            (staging / "lib").symlink_to("usr/lib")
            status, metadata = [], []
            for archive, package in zip(downloads, packages):
                paragraph, files = extract_package(archive, package, staging)
                status.append(paragraph)
                metadata.extend(files)
            make_symlinks_relative(staging)
            database = staging / "var/lib/dpkg"
            (database / "status").write_text("\n".join(status))
            (database / "arch").write_text("arm64\n")
            (database / "info/format").write_text("1\n")
            (staging / ".camera-sysroot.json").write_text(json.dumps(
                {"identity": identity, "metadata": metadata}, indent=2) + "\n")
            if not ready(staging, identity):
                raise ValueError("Prepared sysroot is missing required development files")
            # Publish only a complete sysroot, retaining the previous one on error.
            previous = Path(temporary) / "previous"
            if output.exists():
                output.rename(previous)
            try:
                staging.rename(output)
            except OSError:
                if previous.exists():
                    previous.rename(output)
                raise
        print(f"Prepared CM0 camera sysroot ({len(packages)} locked packages): {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare(args.lock, args.output)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"CM0 sysroot preparation failed: {error}\n")
