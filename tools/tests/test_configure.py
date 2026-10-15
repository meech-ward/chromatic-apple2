#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Readable private password defaults, with custom credentials preserved."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import re
import stat
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('configure', ROOT / 'tools/configure.py')
configure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(configure)


class ConfigureTests(unittest.TestCase):
    def test_generated_passwords_use_shared_words_and_four_digits(self):
        words = configure.PASSWORD_WORDS
        self.assertEqual(len(words), 256)
        self.assertEqual(len(set(words)), 256)
        self.assertTrue(all(re.fullmatch(r'[A-Z]{4}', word) for word in words))
        with patch.object(configure.secrets, 'choice', side_effect=['MINT', 'LAKE']), \
             patch.object(configure.secrets, 'randbelow', return_value=0) as suffix:
            self.assertEqual(configure.generate_password(), 'MINT-LAKE-0000')
            suffix.assert_called_once_with(10000)
        with patch.object(configure.secrets, 'randbelow', return_value=9999):
            self.assertTrue(configure.generate_password().endswith('-9999'))
        for _ in range(200):
            password = configure.generate_password()
            self.assertRegex(password, r'^[A-Z]{4}-[A-Z]{4}-[0-9]{4}$')
            first, second, _ = password.split('-')
            self.assertIn(first, words)
            self.assertIn(second, words)

    def run_configure(self, mode, passwords, generated):
        with tempfile.TemporaryDirectory(prefix='chromatic-configure-test-') as temporary:
            path = Path(temporary) / 'private/settings.json'
            output = io.StringIO()
            with patch('builtins.input', return_value='Test network'), \
                 patch.object(configure.getpass, 'getpass', side_effect=passwords), \
                 patch.object(configure, 'generate_password', side_effect=generated) as generate, \
                 patch.object(configure.secrets, 'token_urlsafe', return_value='test-api-token'), \
                 contextlib.redirect_stdout(output):
                configure.main(['--output', str(path), '--wifi-mode', mode])
            values = json.loads(path.read_text())
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(values['api_token'], 'test-api-token')
            for password in [values['ssh_password'], values['wifi_password']]:
                if password:
                    self.assertNotIn(password, output.getvalue())
            return values, generate.call_count

    def test_blank_ssh_password_generates_phrase_without_changing_open_station(self):
        values, count = self.run_configure('station', ['', ''], ['MINT-LAKE-4826'])
        self.assertEqual(values['wifi_password'], '')
        self.assertEqual(values['ssh_password'], 'MINT-LAKE-4826')
        self.assertEqual(count, 1)

    def test_generated_hotspot_and_ssh_passwords_are_independent(self):
        values, count = self.run_configure('hotspot', ['', ''],
                                           ['MINT-LAKE-4826', 'WAVE-WOLF-1357'])
        self.assertEqual(values['wifi_password'], 'MINT-LAKE-4826')
        self.assertEqual(values['ssh_password'], 'WAVE-WOLF-1357')
        self.assertEqual(count, 2)

    def test_custom_passwords_are_preserved(self):
        values, count = self.run_configure('hotspot',
                                           ['custom hotspot!234', 'Custom SSH!234'], [])
        self.assertEqual(values['wifi_password'], 'custom hotspot!234')
        self.assertEqual(values['ssh_password'], 'Custom SSH!234')
        self.assertEqual(count, 0)


if __name__ == '__main__':
    unittest.main()
