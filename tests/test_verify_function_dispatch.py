"""Regression tests for build failures caused by missing guest dispatch entries."""

import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import subprocess

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_function_dispatch as check


class DispatchVerificationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.manifest = self.root / 'naruto.toml'
        self.registration = self.root / 'naruto_register.cpp'
        self.manifest.write_text('[entrypoint.functions]\n0x825A7428 = { size = 16 }\n')
        self.registration.write_text('registrar->SetFunction(0x825A7428, sub_825A7428);\n')

    def test_missing_dispatch_reports_address_and_fails_cli(self):
        self.registration.write_text('// registrar->SetFunction(0x825A7428, sub_825A7428);\n')
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            status = check.main(['--manifest', str(self.manifest),
                                 '--registration', str(self.registration)])
        self.assertEqual(status, 1)
        self.assertIn('0x825A7428', error.getvalue())

    def test_compiled_symbol_is_required_not_just_source_registration(self):
        # A different function in a stale binary cannot satisfy this entry.
        result = subprocess.CompletedProcess([], 0, 'sub_825A7058 W 1234 20\n', '')
        with patch.object(check.subprocess, 'run', return_value=result):
            with self.assertRaisesRegex(check.VerificationError, 'Missing compiled functions: 0x825A7428'):
                check.verify(self.manifest, self.registration, self.root / 'naruto')

    def test_actual_registration_name_is_checked(self):
        self.registration.write_text('registrar->SetFunction(0x825A7428, custom_callback);\n')
        result = subprocess.CompletedProcess([], 0, 'custom_callback T 1234 20\n', '')
        with patch.object(check.subprocess, 'run', return_value=result):
            self.assertEqual(check.verify(self.manifest, self.registration, self.root / 'naruto'), 1)

    def test_data_symbol_does_not_count_as_function(self):
        result = subprocess.CompletedProcess([], 0, 'sub_825A7428 D 1234 20\n', '')
        with patch.object(check.subprocess, 'run', return_value=result):
            with self.assertRaises(check.VerificationError):
                check.verify(self.manifest, self.registration, self.root / 'naruto')

    def test_chunks_require_parent_not_independent_registration(self):
        self.manifest.write_text('[entrypoint.functions]\n0x825A7428 = { parent = 0x825A7488, size = 16 }\n')
        self.registration.write_text('registrar->SetFunction(0x825A7488, sub_825A7488);\n')
        self.assertEqual(check.verify(self.manifest, self.registration), 1)
        self.registration.write_text('')
        with self.assertRaisesRegex(check.VerificationError, '0x825A7488'):
            check.verify(self.manifest, self.registration)

    def test_included_functions_and_inline_overrides_are_checked(self):
        (self.root / 'callbacks.toml').write_text(
            '[functions]\n0x825A7428 = { parent = 0x825A7488 }\n0x8263B1E0 = { size = 16 }\n')
        self.manifest.write_text('[entrypoint]\nincludes = ["callbacks.toml"]\n'
                                 '[entrypoint.functions]\n0x825A7428 = { size = 16 }\n')
        self.registration.write_text(self.registration.read_text() +
                                      'registrar->SetFunction(0x8263B1E0, sub_8263B1E0);\n')
        self.assertEqual(check.verify(self.manifest, self.registration), 2)

    def test_duplicate_registration_is_rejected(self):
        self.registration.write_text(self.registration.read_text() * 2)
        with self.assertRaisesRegex(check.VerificationError, 'Duplicate dispatch registration'):
            check.verify(self.manifest, self.registration)

    def test_nm_failure_cannot_pass_verification(self):
        result = subprocess.CompletedProcess([], 1, '', 'invalid ELF file')
        with patch.object(check.subprocess, 'run', return_value=result):
            with self.assertRaisesRegex(check.VerificationError, 'nm failed'):
                check.verify(self.manifest, self.registration, self.root / 'naruto')


if __name__ == '__main__':
    unittest.main()
