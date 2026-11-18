"""Exercise sysroot preparation with local Debian fixtures, without a network."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "tools/prepare-cm0-sysroot.py"
SPEC = importlib.util.spec_from_file_location("prepare_cm0_sysroot", SCRIPT)
sysroot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sysroot)


@unittest.skipUnless(shutil.which("dpkg-deb"), "dpkg-deb is required")
class SysrootTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.root = self.work / "cache/sysroot"
        payload = self.work / "package"
        control = payload / "DEBIAN"
        control.mkdir(parents=True)
        (control / "control").write_text(
            "Package: libcamera0.8\nVersion: 0.8.0-1\nArchitecture: arm64\n"
            "Multi-Arch: same\nMaintainer: Test <test@example.com>\n"
            "Description: Test fixture\n")
        (control / "symbols").write_text("libcamera.so.0.8 libcamera0.8 #MINVER#\n")
        (control / "shlibs").write_text("libcamera 0.8 libcamera0.8 (>= 0.8.0)\n")
        self.sentinel = self.work / "postinst-ran"
        (control / "postinst").write_text(f'#!/bin/sh\ntouch "{self.sentinel}"\n')
        (control / "postinst").chmod(0o755)
        for name in sysroot.REQUIRED_FILES:
            if name.startswith("var/"):
                continue
            target = payload / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("fixture\n")
        (payload / "usr/lib/absolute-link").symlink_to(
            "/usr/lib/aarch64-linux-gnu/libcamera.so")
        archive = self.work / "fixture.deb"
        subprocess.run(["dpkg-deb", "--root-owner-group", "--build", str(payload), str(archive)],
                       check=True, stdout=subprocess.DEVNULL, env=sysroot.ENVIRONMENT)
        self.package = {"Package": "libcamera0.8", "Version": "0.8.0-1",
                        "Architecture": "arm64", "SHA256": sysroot.checksum(archive),
                        "URL": "https://invalid.example/never-download.deb"}
        archives = self.root.parent / "archives"
        archives.mkdir(parents=True)
        shutil.copyfile(archive, archives / f"{self.package['SHA256']}.deb")
        self.lock = self.work / "packages.json"
        self.write_lock()

    def write_lock(self):
        self.lock.write_text(json.dumps({"schema_version": 1, "architecture": "arm64",
                                        "packages": [self.package]}))

    def test_extracts_target_metadata_without_running_scripts(self):
        sysroot.prepare(self.lock, self.root)
        info = self.root / "var/lib/dpkg/info"
        self.assertIn("Status: install ok installed", (info.parent / "status").read_text())
        self.assertIn("/usr/lib/aarch64-linux-gnu/libcamera.so",
                      (info / "libcamera0.8:arm64.list").read_text())
        self.assertTrue((info / "libcamera0.8:arm64.symbols").is_file())
        self.assertTrue((info / "libcamera0.8:arm64.shlibs").is_file())
        self.assertFalse(self.sentinel.exists())
        self.assertFalse((self.root / "DEBIAN").exists())
        self.assertFalse(os.readlink(self.root / "usr/lib/absolute-link").startswith("/"))
        self.assertTrue((self.root / "lib/absolute-link").resolve().is_relative_to(self.root.resolve()))

    def test_cached_sysroot_needs_no_download(self):
        sysroot.prepare(self.lock, self.root)
        with patch.object(sysroot, "fetch_archive", side_effect=AssertionError("Unexpected download")):
            sysroot.prepare(self.lock, self.root)

    def test_missing_symbols_trigger_repair(self):
        sysroot.prepare(self.lock, self.root)
        symbols = self.root / "var/lib/dpkg/info/libcamera0.8:arm64.symbols"
        symbols.unlink()
        sysroot.prepare(self.lock, self.root)
        self.assertTrue(symbols.is_file())

    def test_wrong_package_keeps_previous_sysroot(self):
        sysroot.prepare(self.lock, self.root)
        previous = (self.root / ".camera-sysroot.json").read_bytes()
        self.package["Version"] = "0.9.0-1"
        self.write_lock()
        with self.assertRaisesRegex(ValueError, "identity does not match"):
            sysroot.prepare(self.lock, self.root)
        self.assertEqual(previous, (self.root / ".camera-sysroot.json").read_bytes())

    def test_refuses_unmanaged_directory(self):
        self.root.mkdir()
        with self.assertRaisesRegex(ValueError, "unmanaged"):
            sysroot.prepare(self.lock, self.root)

    def test_bad_download_is_not_cached(self):
        archive = self.root.parent / "archives" / f"{self.package['SHA256']}.deb"
        archive.unlink()

        def corrupt_download(command, **kwargs):
            Path(command[command.index("--output") + 1]).write_bytes(b"corrupt")

        with patch.object(sysroot.subprocess, "run", side_effect=corrupt_download):
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                sysroot.fetch_archive(self.package, archive.parent)
        self.assertFalse(archive.exists())
        self.assertFalse(archive.with_suffix(".partial").exists())


if __name__ == "__main__":
    unittest.main()
