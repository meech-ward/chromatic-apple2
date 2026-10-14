#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Standalone-path and programmer safety checks. No network or USB operations."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True


def module(path):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def inventory(serials):
    rows = [("Bus", "device", "vid:pid", "probe_type", "manufacturer", "serial", "product")]
    rows += [("001", str(index + 2), "0x33aa:0120", "gwu2x", "Example Vendor", serial, "GWU2X")
             for index, serial in enumerate(serials)]
    rows += [("003", "009", "0x0403:6014", "ft232H", "Other Vendor", "other", "Other probe")]
    widths = [max(len(row[index]) for row in rows) + 2 for index in range(6)]
    return "\n".join("".join(value.ljust(width) for value, width in zip(row, widths)) + row[6]
                     for row in rows) + "\n"


class DeviceTests(unittest.TestCase):
    def setUp(self):
        self.device = module(ROOT / "device.py")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name).resolve()
        self.scan = patch.object(self.device.subprocess, "check_output", return_value=inventory(["example-serial"]))
        self.scanner = self.scan.start()
        self.addCleanup(self.scan.stop)
        self.execute = patch.object(self.device.subprocess, "run")
        self.runner = self.execute.start()
        self.addCleanup(self.execute.stop)
        self.lookup = patch.object(self.device.shutil, "which", return_value="/mock/openFPGALoader")
        self.lookup.start()
        self.addCleanup(self.lookup.stop)

    def call(self, arguments, code=None):
        output, error = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(error):
            if code is None:
                self.device.main(arguments)
            else:
                with self.assertRaises(SystemExit) as exited:
                    self.device.main(arguments)
                self.assertEqual(exited.exception.code, code)
        return output.getvalue(), error.getvalue()

    def image(self, suffix):
        image = self.directory / ("example" + suffix)
        image.write_bytes(b"parser fixture, never a real bitstream")
        return image, hashlib.sha256(image.read_bytes()).hexdigest()

    def test_status_handles_arbitrary_serial_and_ansi_table(self):
        self.scanner.return_value = "\x1b[32m" + inventory(["owner-specific"]) + "\x1b[0m"
        output, _ = self.call(["status"])
        devices = json.loads(output)["programmers"]
        self.assertEqual([d["programmer_serial"] for d in devices], ["owner-specific"])
        self.scanner.assert_called_once_with(["/mock/openFPGALoader", "--scan-usb"],
                                             text=True, stderr=self.device.subprocess.STDOUT, timeout=20)
        self.runner.assert_not_called()

    def test_all_operations_require_explicit_serial_before_usb_scan(self):
        self.call(["restore"], 2)
        self.scanner.assert_not_called()
        self.runner.assert_not_called()

    def test_rejects_ambiguous_or_mismatched_programmer(self):
        self.scanner.return_value = inventory(["example-serial", "example-serial"])
        self.call(["--serial", "example-serial", "restore"], 1)
        self.scanner.return_value = inventory(["different-serial"])
        self.call(["--serial", "example-serial", "restore"], 1)
        self.scanner.return_value = "unrecognized scan output"
        self.call(["--serial", "example-serial", "restore"], 1)
        self.runner.assert_not_called()

    def test_hash_guard_precedes_any_usb_scan(self):
        image, _ = self.image(".fs")
        self.call(["--serial", "example-serial", "load", str(image), "--sha256", "0" * 64], 1)
        self.scanner.assert_not_called()
        self.runner.assert_not_called()

    def test_sram_and_raw_backup_install_commands_are_distinct(self):
        image, digest = self.image(".fs")
        self.call(["--serial", "example-serial", "load", str(image), "--sha256", digest])
        command = self.runner.call_args.args[0]
        self.assertIn("--write-sram", command)
        self.assertNotIn("--write-flash", command)
        self.call(["--serial", "example-serial", "install", str(image), "--sha256", digest])
        command = self.runner.call_args.args[0]
        self.assertIn("--write-flash", command)
        self.assertNotIn("--external-flash", command)
        image, digest = self.image(".bin")
        self.call(["--serial", "example-serial", "install", str(image), "--sha256", digest])
        command = self.runner.call_args.args[0]
        self.assertEqual(command, ["/mock/openFPGALoader", "--cable", "gwu2x", "--external-flash", "--write-flash",
                                  "--offset", "0", "--verify", "--reset", "--file-type", "bin", str(image)])

    def test_backup_never_clobbers_existing_file_or_symlink(self):
        target = self.directory / "factory.bin"
        target.write_bytes(b"keep")
        self.call(["--serial", "example-serial", "backup", str(target), "--size", "4"], 1)
        link = self.directory / "link.bin"
        link.symlink_to(target)
        self.call(["--serial", "example-serial", "backup", str(link), "--size", "4"], 1)
        self.assertEqual(target.read_bytes(), b"keep")
        self.scanner.assert_not_called()
        self.runner.assert_not_called()


class RelocatedBuildTests(unittest.TestCase):
    def test_build_and_setup_resolve_their_own_checkout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve() / "standalone checkout"
            tools = root / "tools"
            tools.mkdir(parents=True)
            for name in ("fpga_build.py", "setup_fpga_tools.py", "smoke.v", "smoke.cst"):
                shutil.copyfile(ROOT / "tools" / name, tools / name)
            suite = root / "build/tools/oss-cad-suite"
            (suite / "bin").mkdir(parents=True)
            (suite / "bin/yosys").touch()
            build = module(tools / "fpga_build.py")
            calls = []

            def fake_run(command, log, directory, environment, timeout):
                calls.append(command)
                self.assertEqual(directory, root / "build/tools/smoke")
                self.assertTrue(environment["XDG_CONFIG_HOME"].startswith(str(root)))
                log.write_text("mocked tool; no process executed\n")
                (directory / "synth.json").write_text("{}")
                (directory / "routed.json").write_text("{}")
                (directory / "design.fs").write_bytes(b"mocked image")

            previous = Path.cwd()
            try:
                os.chdir(temporary)
                with patch.object(build, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()):
                    build.main(["--param", "EXAMPLE=0x10"])
            finally:
                os.chdir(previous)
            manifest = json.loads((root / "build/tools/smoke/build.json").read_text())
            self.assertEqual(manifest["parameters"], {"EXAMPLE": 16})
            self.assertFalse(manifest["hardware_programmed"])
            self.assertEqual(len(calls), 3)
            self.assertIn(str(tools / "smoke.v"), manifest["inputs"])
            setup = module(tools / "setup_fpga_tools.py")
            archive = root / "build/tools/downloads" / setup.ARCHIVE
            archive.parent.mkdir()
            archive.write_bytes(b"mocked pinned archive")
            output = io.StringIO()
            with patch.object(setup.platform, "system", return_value="Darwin"), \
                 patch.object(setup.platform, "machine", return_value="arm64"), \
                 patch.object(setup, "SHA256", hashlib.sha256(archive.read_bytes()).hexdigest()), \
                 patch.object(setup.urllib.request, "urlretrieve") as download, \
                 patch.object(setup.subprocess, "run") as extract, contextlib.redirect_stdout(output):
                setup.main([])
                download.assert_not_called()
                extract.assert_not_called()
            self.assertIn(str(tools / "fpga_build.py"), output.getvalue())


class ClientTests(unittest.TestCase):
    def test_upload_and_redaction_use_explicit_host_and_local_config(self):
        client = module(ROOT / "client.py")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            config = directory / "private.json"
            config.write_text(json.dumps({"api_token": "test-token-only", "ssh_password": "test-password-only"}))
            disk = directory / "example.dsk"
            disk.write_bytes(b"\0" * 143360)
            response = Mock()
            response.read.return_value = b"test-token-only test-password-only uploaded"
            response.__enter__ = Mock(return_value=response)
            response.__exit__ = Mock(return_value=False)
            opener = Mock()
            opener.open.return_value = response
            output = io.StringIO()
            with patch.object(client, "build_opener", return_value=opener), contextlib.redirect_stdout(output):
                client.main(["--host", "127.0.0.1:8123", "--config", str(config), "upload", str(disk)])
            request = opener.open.call_args.args[0]
            self.assertEqual(request.full_url, "http://127.0.0.1:8123/disk?name=Workshop.dsk")
            self.assertEqual(len(request.data), 143360)
            self.assertEqual(opener.open.call_args.kwargs["timeout"], 900)
            self.assertNotIn("test-token-only", output.getvalue())
            self.assertNotIn("test-password-only", output.getvalue())

    def test_invalid_private_config_exits_without_request(self):
        client = module(ROOT / "client.py")
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "private.json"
            config.write_text("[]")
            with patch.object(client, "build_opener") as opener, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as exited:
                    client.main(["--host", "127.0.0.1", "--config", str(config), "status"])
                self.assertEqual(exited.exception.code, 2)
                opener.assert_not_called()


if __name__ == "__main__":
    unittest.main()
