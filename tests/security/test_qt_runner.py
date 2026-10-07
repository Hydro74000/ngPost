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
    def run_fixture(self, log, name='tst_RunnerFixture', platform='Linux', code=0, counts_crlf=False):
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
            runner = root / '.github/scripts/run-qt-tests.sh'
            runner.parent.mkdir(parents=True)
            runner.write_bytes(RUNNER.read_bytes())
            counts = root / 'tests/expected-counts.txt'
            counts.parent.mkdir()
            table = (RUNNER.parents[2] / 'tests/expected-counts.txt').read_text()
            counts.write_bytes(table.replace('\n', '\r\n' if counts_crlf else '\n').encode())
            return subprocess.run(['bash', str(runner), str(tests), 'fixture'],
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
                    allowed = platform != 'MINGW64_NT-10.0' and name == 'tst_WindowsBindHelper'
                    self.assertEqual(self.run_fixture(SKIP, name, platform).returncode == 0, allowed)
        self.assertNotEqual(self.run_fixture(None, 'tst_WindowsSecurity').returncode, 0)
        self.assertNotEqual(self.run_fixture(SKIP, 'tst_WindowsSecurity', code=1).returncode, 0)

    def test_pinned_minimum_rejects_lost_tests_and_accepts_growth(self):
        # Use the real table so a renamed/missing entry cannot silently disable
        # enforcement. The generic log-shape fixtures above stay unpinned.
        counts = RUNNER.parents[2] / 'tests/expected-counts.txt'
        minimum = next(int(parts[1]) for line in counts.read_text().splitlines()
                       if (parts := line.split()) and parts[0] == 'tst_CliParser'
                       and parts[2:] == ['Linux'])
        for passed in (minimum - 1, minimum, minimum + 1):
            with self.subTest(passed=passed):
                log = PASS.replace('3 passed', f'{passed} passed')
                result = self.run_fixture(log, 'tst_CliParser')
                self.assertEqual(result.returncode == 0, passed >= minimum, result.stdout)
                if passed < minimum:
                    self.assertIn('below the', result.stdout)

    @staticmethod
    def linux_only_binary():
        # Read from the table rather than named here: pinning that binary on
        # another platform later must break this test, not silently empty it.
        platforms = {}
        counts = RUNNER.parents[2] / 'tests/expected-counts.txt'
        for line in counts.read_text().splitlines():
            parts = line.split()
            if not parts or parts[0].startswith('#'):
                continue
            platforms.setdefault(parts[0], set()).add(parts[2] if len(parts) > 2 else '')
        for binary, pinned in sorted(platforms.items()):
            if pinned == {'Linux'}:
                return binary
        raise AssertionError('no binary is pinned on Linux only any more')

    def test_linux_floor_does_not_apply_to_other_platforms(self):
        name = self.linux_only_binary()
        for platform in ('Darwin', 'MINGW64_NT-10.0', 'MSYS_NT-10.0', 'CYGWIN_NT-10.0'):
            with self.subTest(platform=platform):
                result = self.run_fixture(PASS, name + '.exe', platform)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn('no pinned minimum', result.stdout)

    def test_windows_floor_normalizes_platform_and_executable_suffix(self):
        counts = RUNNER.parents[2] / 'tests/expected-counts.txt'
        minimum = next(int(parts[1]) for line in counts.read_text().splitlines()
                       if (parts := line.split()) and parts[0] == 'tst_WindowsSecurity'
                       and parts[2:] == ['Windows'])
        for platform in ('MINGW64_NT-10.0', 'MSYS_NT-10.0', 'CYGWIN_NT-10.0', 'Windows_NT'):
            for passed in (minimum - 1, minimum):
                with self.subTest(platform=platform, passed=passed):
                    result = self.run_fixture(PASS.replace('3 passed', f'{passed} passed'),
                                              'tst_WindowsSecurity.exe', platform)
                    self.assertEqual(result.returncode == 0, passed >= minimum, result.stdout)

    def test_crlf_checkout_does_not_disable_windows_floors(self):
        counts = RUNNER.parents[2] / 'tests/expected-counts.txt'
        minimum = next(int(parts[1]) for line in counts.read_text().splitlines()
                       if (parts := line.split()) and parts[0] == 'tst_WindowsSecurity'
                       and parts[2:] == ['Windows'])
        for passed in (minimum - 1, minimum):
            with self.subTest(passed=passed):
                result = self.run_fixture(PASS.replace('3 passed', f'{passed} passed'),
                                          'tst_WindowsSecurity.exe', 'MINGW64_NT-10.0',
                                          counts_crlf=True)
                self.assertEqual(result.returncode == 0, passed >= minimum, result.stdout)
                self.assertNotIn('no pinned minimum', result.stdout)

    def test_windows_acl_skip_reports_the_elevation_prerequisite(self):
        log = PASS.replace('Totals:', 'SKIP   : Test::acl_case() NTFS ownership fixtures require an elevated Windows test process\nTotals:').replace('0 skipped', '1 skipped')
        result = self.run_fixture(log, 'tst_WindowsSecurity.exe', 'Windows_NT')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Check SKIP reasons and prerequisites first', result.stdout)
        self.assertIn('Windows ACL tests require an elevated process', result.stdout)
