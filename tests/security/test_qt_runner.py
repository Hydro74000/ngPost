"""Exercise the actual CI runner with disposable fake QTest executables."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

RUNNER = Path(__file__).resolve().parents[2] / '.github/scripts/run-qt-tests.sh'
PASS = 'PASS   : Test::real_case()\nTotals: 3 passed, 0 failed, 0 skipped, 0 blacklisted, 1ms\n'
SKIP = 'SKIP   : Test::initTestCase() Windows-only\nTotals: 0 passed, 0 failed, 1 skipped, 0 blacklisted, 1ms\n'


class QtRunnerTests(unittest.TestCase):
    def run_fixture(self, log, name='tst_CliParser', platform='Linux', code=0):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tests = root / 'tests with spaces'
            tests.mkdir()
            binary = tests / name
            script = '#!/bin/bash\n'
            if log is not None:
                script += 'printf %s ' + shlex.quote(log) + ' > "${2%,txt}"\n'
            script += 'exit ' + str(code) + '\n'
            binary.write_text(script)
            binary.chmod(0o755)
            uname = root / 'uname'
            uname.write_text('#!/bin/bash\nprintf "%s\\n" ' + shlex.quote(platform) + '\n')
            uname.chmod(0o755)
            return subprocess.run(['bash', str(RUNNER), str(tests), 'fixture'],
                                  env=dict(os.environ, PATH=str(root) + os.pathsep + os.environ['PATH']),
                                  capture_output=True, text=True)

    def test_successful_body_is_required(self):
        self.assertEqual(self.run_fixture(PASS).returncode, 0)
        for log in (None, '', SKIP, 'incomplete log',
                    'PASS   : Test::initTestCase()\nPASS   : Test::cleanupTestCase()\n'
                    'Totals: 2 passed, 0 failed, 8 skipped, 0 blacklisted, 1ms\n',
                    PASS + PASS, PASS.replace('0 failed', '1 failed')):
            with self.subTest(log=log):
                self.assertNotEqual(self.run_fixture(log).returncode, 0)
        self.assertNotEqual(self.run_fixture(PASS, code=1).returncode, 0)

    def test_empty_exception_is_narrow_and_platform_specific(self):
        for platform in ('Linux', 'Darwin', 'MINGW64_NT-10.0'):
            for name in ('tst_WindowsSecurity', 'tst_WindowsBindHelper',
                         'tst_WindowsCommandLine', 'tst_CliParser'):
                with self.subTest(platform=platform, name=name):
                    allowed = platform != 'MINGW64_NT-10.0' and name in (
                        'tst_WindowsSecurity', 'tst_WindowsBindHelper')
                    self.assertEqual(self.run_fixture(SKIP, name, platform).returncode == 0, allowed)
        self.assertNotEqual(self.run_fixture(None, 'tst_WindowsSecurity').returncode, 0)
        self.assertNotEqual(self.run_fixture(SKIP, 'tst_WindowsSecurity', code=1).returncode, 0)
