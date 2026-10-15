# SPDX-License-Identifier: MIT
"""Validate runtime settings and NVS failure recovery without hardware."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WifiSettingsTests(unittest.TestCase):
    def test_profiles_and_atomic_store(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        with tempfile.TemporaryDirectory(prefix='chromatic-wifi-test-') as temporary:
            for name in ('wifi_settings', 'wifi_store', 'wifi_setup_key',
                         'wifi_setup_password'):
                binary = Path(temporary) / name
                sources = [ROOT / 'mcu/main/wifi_setup_password.c']
                if name != 'wifi_setup_password':
                    sources.append(ROOT / 'mcu/main/wifi_settings.c')
                if name in ('wifi_store', 'wifi_setup_key'):
                    sources.append(ROOT / 'mcu/main/wifi_store.c')
                subprocess.run([*compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                '-I' + str(ROOT / 'mcu/main'),
                                '-I' + str(ROOT / 'mcu/tests/wifi_stubs'),
                                *map(str, sources), str(ROOT / f'mcu/tests/{name}_test.c'),
                                '-o', str(binary)], check=True, timeout=30)
                result = subprocess.run([str(binary)], check=True, capture_output=True,
                                        text=True, timeout=30)
                self.assertIn('PASS ', result.stdout)


if __name__ == '__main__':
    unittest.main()
