#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import stat
from unittest.mock import patch
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('check_public', ROOT / 'tools/check_public.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
config_spec = importlib.util.spec_from_file_location('configure', ROOT / 'tools/configure.py')
configure = importlib.util.module_from_spec(config_spec)
config_spec.loader.exec_module(configure)


class ReleaseInputs(unittest.TestCase):
    def test_default_build_has_no_network_credentials(self):
        with tempfile.TemporaryDirectory() as work:
            output = Path(work) / 'private_config.h'
            subprocess.run([sys.executable, str(ROOT / 'mcu/tools/embed_config.py'),
                            '--output', str(output)], check=True, capture_output=True)
            self.assertIn('DEVICE_WIFI_ENABLED 0', output.read_text())
            self.assertIn('DEVICE_WIFI_AP 0', output.read_text())
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

    def test_mode_defaults_and_hotspot_password_validation(self):
        with tempfile.TemporaryDirectory() as work:
            settings = Path(work) / 'settings.json'
            values = dict(wifi_ssid='unit-test-network', wifi_password='unit-test-password',
                          api_token='unit-test-token-' + 'a' * 32, ssh_password='unit-test-ssh-password')
            output = Path(work) / 'private_config.h'
            command = [sys.executable, str(ROOT / 'mcu/tools/embed_config.py'),
                       '--input', str(settings), '--output', str(output)]

            def build(expected, mode, password):
                if mode is not None:
                    values['wifi_mode'] = mode
                else:
                    values.pop('wifi_mode', None)
                values['wifi_password'] = password
                settings.write_text(json.dumps(values))
                output.unlink(missing_ok=True)
                result = subprocess.run(command, capture_output=True)
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertNotIn(password.encode() if password else b'NO_SECRET',
                                 result.stdout + result.stderr)
                self.assertEqual(output.exists(), expected == 0)

            build(0, None, 'unit-test-password')
            self.assertIn('DEVICE_WIFI_AP 0', output.read_text())
            build(0, 'station', '')  # Existing open-network station settings remain valid.
            build(0, 'hotspot', 'unit-test-password')
            self.assertIn('DEVICE_WIFI_AP 1', output.read_text())
            build(0, 'hotspot', 'x' * 63)
            for mode, password in [('automatic', 'unit-test-password'), ('hotspot', ''),
                                   ('hotspot', 'short'), ('hotspot', 'x' * 64),
                                   ('hotspot', 'nonascii-\u00e9'), ('hotspot', 'line\nbreak')]:
                with self.subTest(mode=mode, password_length=len(password)):
                    build(2, mode, password)

    def test_configure_generates_private_hotspot_and_preserves_station_default(self):
        with tempfile.TemporaryDirectory() as work:
            for mode in ('station', 'hotspot'):
                output = Path(work) / (mode + '.json')
                arguments = ['configure.py', '--output', str(output)]
                if mode == 'hotspot':
                    arguments += ['--wifi-mode', mode]
                with patch.object(sys, 'argv', arguments), patch('builtins.input', return_value='unit-test-network'), \
                     patch.object(configure.getpass, 'getpass', side_effect=['', '']), \
                     patch('builtins.print'):
                    configure.main()
                values = json.loads(output.read_text())
                self.assertEqual(values['wifi_mode'], mode)
                self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
                self.assertGreaterEqual(len(values['ssh_password']), 8)
                if mode == 'hotspot':
                    self.assertTrue(8 <= len(values['wifi_password']) <= 63)
                else:
                    self.assertEqual(values['wifi_password'], '')

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


    def test_source_guard_readme_image_keeps_private_checks(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            image = root / release.README_IMAGE
            image.parent.mkdir()
            image.write_bytes(release.PNG_SIGNATURE + b'test-image')
            errors = release.problems(root, [Path(release.README_IMAGE)])
            self.assertFalse(any(release.README_IMAGE in e for e in errors))
            image.write_bytes(release.PNG_SIGNATURE + b'PRIVATE_IMAGE_VALUE_123456')
            errors = release.problems(root, [Path(release.README_IMAGE)],
                                      ['PRIVATE_IMAGE_VALUE_123456'])
            self.assertTrue(any('private setting' in e for e in errors))
            image.write_bytes(release.PNG_SIGNATURE + bytes(2 * 1024 * 1024))
            errors = release.problems(root, [Path(release.README_IMAGE)])
            self.assertTrue(any('no larger than' in e for e in errors))
            other = image.parent / 'other.png'
            other.write_bytes(release.PNG_SIGNATURE)
            errors = release.problems(root, [other.relative_to(root)])
            self.assertTrue(any('UTF-8' in e for e in errors))

if __name__ == '__main__':
    unittest.main()
