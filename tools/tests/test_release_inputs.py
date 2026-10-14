#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('check_public', ROOT / 'tools/check_public.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseInputs(unittest.TestCase):
    def test_default_build_has_no_network_credentials(self):
        with tempfile.TemporaryDirectory() as work:
            output = Path(work) / 'private_config.h'
            subprocess.run([sys.executable, str(ROOT / 'mcu/tools/embed_config.py'),
                            '--output', str(output)], check=True, capture_output=True)
            self.assertIn('DEVICE_WIFI_ENABLED 0', output.read_text())
            self.assertIn('DEVICE_SSH_ENABLED 0', output.read_text())
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)

    def test_example_cannot_be_used_as_live_credentials(self):
        with tempfile.TemporaryDirectory() as work:
            result = subprocess.run([sys.executable, str(ROOT / 'mcu/tools/embed_config.py'),
                                     '--input', str(ROOT / 'examples/device-local.example.json'),
                                     '--output', str(Path(work) / 'output.h')], capture_output=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn(b'example placeholders', result.stderr)
            self.assertFalse((Path(work) / 'output.h').exists())

    def test_private_header_and_invalid_header_token(self):
        with tempfile.TemporaryDirectory() as work:
            settings = Path(work) / 'settings.json'
            values = dict(wifi_ssid='unit-test-network', wifi_password='unit-test-password',
                          api_token='unit-test-token-' + 'a' * 32, ssh_password='unit-test-ssh-password')
            settings.write_text(json.dumps(values))
            output = Path(work) / 'private_config.h'
            command = [sys.executable, str(ROOT / 'mcu/tools/embed_config.py'),
                       '--input', str(settings), '--output', str(output)]
            result = subprocess.run(command, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(values['ssh_password'].encode(), result.stdout + result.stderr)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
            values['api_token'] += '\r\nInjected: no'
            settings.write_text(json.dumps(values))
            result = subprocess.run(command, capture_output=True)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn(values['api_token'].encode(), result.stderr)

    def test_source_guard_rejects_private_content_and_artifacts(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            (root / 'example.py').write_text('password = "PRIVATE_TEST_VALUE_123456"\n')
            (root / 'disk.dsk').write_bytes(b'private disk')
            errors = release.problems(root, [Path('example.py'), Path('disk.dsk')],
                                      ['PRIVATE_TEST_VALUE_123456'])
            self.assertTrue(any('private setting' in e for e in errors))
            self.assertTrue(any('artifact' in e for e in errors))
            self.assertFalse(any('PRIVATE_TEST_VALUE_123456' in e for e in errors))


if __name__ == '__main__':
    unittest.main()
