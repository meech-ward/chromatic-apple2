#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the actual portable portal renderer and its HTML form contract."""
from html.parser import HTMLParser
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Form(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tags = []

    def handle_starttag(self, tag, attrs):
        self.tags.append((tag, dict(attrs)))


class WifiPortal(unittest.TestCase):
    def test_renderer_and_html_form(self):
        compiler = shlex.split(os.environ.get('CC', 'cc'))
        self.assertTrue(compiler and shutil.which(compiler[0]), 'A C compiler is required')
        with tempfile.TemporaryDirectory(prefix='chromatic-portal-test-') as temporary:
            binary = Path(temporary) / 'portal'
            subprocess.run([*compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'mcu/main'),
                            str(ROOT / 'mcu/main/wifi_portal.c'),
                            str(ROOT / 'mcu/tests/wifi_portal_test.c'), '-o', str(binary)],
                           check=True, capture_output=True, timeout=30)
            run = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
            self.assertIn('PASS Wi-Fi portal', run.stdout)
            ssid = "Home <img src=x> & \"'"
            html = subprocess.run([str(binary), ssid], check=True, capture_output=True,
                                  text=True, timeout=10).stdout
            form = Form()
            form.feed(html)
            self.assertFalse(any(tag in ('img', 'iframe', 'link') for tag, _ in form.tags))
            self.assertFalse(any('src' in attrs for _, attrs in form.tags))
            fields = {attrs.get('name'): attrs for tag, attrs in form.tags if tag in ('input', 'select')}
            self.assertEqual(fields['ssid']['value'], ssid)
            self.assertEqual(fields['csrf']['value'], '0123456789abcdef0123456789abcdef')
            self.assertEqual(fields['password']['type'], 'password')
            self.assertNotIn('value', fields['password'])
            self.assertNotIn('open', fields)
            self.assertIn(('form', {'id': 'setup', 'method': 'post', 'action': '/wifi/save'}), form.tags)
            self.assertIn('<noscript>', html)
            self.assertIn('no firmware update is needed', html)
            self.assertLess(len(html.encode()), 6144)


if __name__ == '__main__':
    unittest.main()
