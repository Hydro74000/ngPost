"""Check formatting range selection using real Git history and a fake formatter."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


RUNNER = Path(__file__).resolve().parents[2] / '.github/scripts/check-format.sh'


class FormatRunnerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / 'repo with spaces'
        self.root.mkdir()
        self.git('init', '-q', '-b', 'devel')
        self.git('config', 'user.name', 'Format fixture')
        self.git('config', 'user.email', 'format@example.invalid')
        self.git('config', 'commit.gpgsign', 'false')
        self.source = self.root / 'legacy.cpp'
        self.source.write_text('// BAD legacy formatting\n')
        self.legacy = self.commit('Legacy code')
        self.style = self.root / '.clang-format'
        self.style.write_text('BasedOnStyle: WebKit\n')
        self.introduction = self.commit('Introduce formatting rules')
        self.runner = self.root / '.github/scripts/check-format.sh'
        self.runner.parent.mkdir(parents=True)
        self.runner.write_bytes(RUNNER.read_bytes())
        tools = self.root / 'tools'
        tools.mkdir()
        self.clang_format = tools / 'clang-format'
        self.clang_format.write_text('#!/bin/bash\necho "clang-format fixture"\n')
        self.clang_format.chmod(0o755)
        self.git_clang_format = tools / 'git-clang-format'
        self.git_clang_format.write_text('''#!/usr/bin/env python3
import os
import subprocess
import sys

diff = subprocess.check_output(
    ['git', 'diff', '--unified=0', sys.argv[-1], '--', 'legacy.cpp'], text=True)
if any(line.startswith('+') and not line.startswith('+++') and 'BAD' in line
       for line in diff.splitlines()):
    print(diff)
    sys.exit(1)
sys.exit(int(os.environ.get('NGPOST_FORMAT_FIXTURE_STATUS', '0')))
''')
        self.git_clang_format.chmod(0o755)

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root, text=True).strip()

    def commit(self, message):
        paths = ['legacy.cpp']
        if (self.root / '.clang-format').exists():
            paths.append('.clang-format')
        self.git('add', *paths)
        self.git('commit', '-q', '-m', message)
        return self.git('rev-parse', 'HEAD')

    def run_fixture(self, base=None, status=0, github_actions=False):
        env = dict(os.environ,
                   CLANG_FORMAT=str(self.clang_format),
                   GIT_CLANG_FORMAT=str(self.git_clang_format),
                   NGPOST_FORMAT_FIXTURE_STATUS=str(status))
        # The runner reports through annotations on GitHub Actions: keep the
        # local mode unless a test asks for it, whatever the host CI is.
        env.pop('GITHUB_ACTIONS', None)
        if github_actions:
            env['GITHUB_ACTIONS'] = 'true'
        return subprocess.run(['bash', str(self.runner), *([] if base is None else [base])],
                              cwd=self.root, env=env, capture_output=True, text=True)

    def assert_base(self, result, base):
        self.assertIn(f'checking lines changed since {base[:7]}', result.stdout)

    def test_release_base_preserves_untouched_legacy_code(self):
        result = self.run_fixture(self.legacy)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('base predates .clang-format', result.stdout)
        self.assert_base(result, self.introduction)

    def test_new_bad_code_still_fails_with_a_release_base(self):
        self.source.write_text('// BAD legacy formatting\n// BAD new formatting\n')
        self.commit('Add unformatted code')
        result = self.run_fixture(self.legacy)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('changed lines do not follow .clang-format', result.stderr)

    def test_github_actions_reports_an_error_annotation(self):
        self.source.write_text('// BAD legacy formatting\n// BAD new formatting\n')
        self.commit('Add unformatted code')
        result = self.run_fixture(self.legacy, github_actions=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('::error title=clang-format::changed lines do not follow .clang-format',
                      result.stdout)

    def test_edits_to_legacy_code_still_fail(self):
        self.source.write_text('// BAD edited legacy formatting\n')
        result = self.run_fixture(self.legacy)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_later_rule_changes_do_not_advance_the_release_base(self):
        self.source.write_text('// BAD legacy formatting\n// BAD new formatting\n')
        self.commit('Add unformatted code')
        self.style.write_text('BasedOnStyle: WebKit\nColumnLimit: 100\n')
        self.commit('Update formatting rules')
        result = self.run_fixture(self.legacy)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assert_base(result, self.introduction)

    def test_base_with_rules_keeps_the_requested_range(self):
        self.source.write_text('// BAD legacy formatting\n// formatted new code\n')
        base = self.commit('Add formatted code')
        self.source.write_text('// BAD legacy formatting\n// formatted updated code\n')
        result = self.run_fixture(base)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('base predates', result.stdout)
        self.assert_base(result, base)
        self.source.write_text('// BAD legacy formatting\n// BAD updated code\n')
        result = self.run_fixture(base)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_diverged_release_branch_uses_rule_introduction_on_devel(self):
        self.git('checkout', '-q', '-b', 'master', self.legacy)
        self.source.write_text('// BAD legacy formatting\n// stable release note\n')
        stable = self.commit('Stable release')
        self.git('checkout', '-q', 'devel')
        result = self.run_fixture(stable)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_base(result, self.introduction)

    def test_unusable_base_checks_the_last_commit(self):
        self.source.write_text('// BAD legacy formatting\n// BAD new formatting\n')
        self.commit('Add unformatted code')
        for base in (None, '0' * 40, 'f' * 40):
            with self.subTest(base=base):
                result = self.run_fixture(base)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('checking the last commit only', result.stdout)
                self.assert_base(result, self.introduction)

    def test_formatter_errors_remain_failures(self):
        result = self.run_fixture(self.legacy, status=2)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('git-clang-format failed (exit 2)', result.stderr)
