#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test real networking code with test-only ESP-IDF I/O, without hardware."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class NetworkModes(unittest.TestCase):
    def test_disabled_station_and_hotspot(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        self.assertTrue(compiler and shutil.which(compiler[0]), 'A C compiler is required')
        with tempfile.TemporaryDirectory(prefix='chromatic-network-test-') as temporary:
            work = Path(temporary)
            for name in ('esp_err.h', 'esp_event.h', 'esp_http_server.h', 'esp_netif.h', 'esp_wifi.h',
                         'nvs_flash.h', 'esp_timer.h', 'esp_random.h'):
                (work / name).write_text('#include "network_sdk.h"\n')
            (work / 'freertos').mkdir()
            for name in ('FreeRTOS.h', 'portmacro.h', 'task.h'):
                (work / 'freertos' / name).write_text('#include \"network_sdk.h\"\n')
            (work / 'lwip').mkdir()
            (work / 'lwip/sockets.h').write_text(
                '#include <sys/socket.h>\n#include <arpa/inet.h>\n#include <netinet/in.h>\n')
            (work / 'private_config.h').write_text(
                '#define DEVICE_WIFI_SSID "unit-test-network"\n'
                '#define DEVICE_WIFI_PASSWORD "unit-test-password"\n'
                '#define DEVICE_API_TOKEN "unit-test-token-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"\n')
            for mode, enabled, access_point in (('disabled', 0, 0), ('station', 1, 0), ('hotspot', 1, 1)):
                with self.subTest(mode=mode):
                    binary = work / mode
                    command = [*compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                               '-I' + str(work), '-I' + str(ROOT / 'mcu/tests/network_stubs'),
                               '-I' + str(ROOT / 'mcu/main'),
                               f'-DDEVICE_WIFI_ENABLED={enabled}', f'-DDEVICE_WIFI_AP={access_point}',
                               str(ROOT / 'mcu/tests/network_test.c'),
                               str(ROOT / 'mcu/main/wifi_settings.c'),
                               str(ROOT / 'mcu/main/wifi_setup_password.c'),
                               str(ROOT / 'mcu/main/wifi_portal.c'), '-o', str(binary)]
                    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    scenarios = ('defaults',) if not enabled else (
                        'defaults', 'stored-station', 'stored-hotspot', 'load-failed', 'power-off')
                    for scenario in scenarios:
                        with self.subTest(scenario=scenario):
                            result = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=10)
                            self.assertEqual(result.returncode, 0, result.stderr)
                            self.assertIn('PASS actual networking', result.stdout)


if __name__ == '__main__':
    unittest.main()
