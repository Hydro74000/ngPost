"""Privileged migration fixture: ONLY run in a disposable, unmounted container.

docker run --rm --network none --memory=256m --cpus=1 \
  -e NGPOST_DISPOSABLE_UPGRADE_TEST=1 -v "$PWD:/audit-input:ro" \
  --entrypoint python3 <local-test-image> /audit-input/tests/security/test_helper_upgrade.py
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get('NGPOST_DISPOSABLE_UPGRADE_TEST') == '1'
                     and Path('/.dockerenv').exists() and hasattr(os, 'getuid')
                     and os.getuid() == 0, 'requires explicit disposable root container')
class HelperUpgradeTests(unittest.TestCase):
    def test_failed_and_successful_migration(self):
        source = Path(__file__).resolve().parents[2] / 'src/vpn'
        installed = Path('/var/lib/ngpost/ngpost-vpn-helper.sh')
        rule = Path('/etc/polkit-1/rules.d/49-ngpost-vpn.rules')
        installed.parent.mkdir(parents=True, exist_ok=True)
        rule.parent.mkdir(parents=True, exist_ok=True)

        def legacy():
            installed.write_text('#!/bin/bash\nreadonly NGPOST_VPN_HELPER_PROTOCOL=2\n')
            installed.chmod(0o755)
            rule.write_text('// legacy passwordless authorization\n')

        def migrate(directory):
            return subprocess.run(['bash', str(source / 'scripts/ngpost-vpn-install.sh'),
                                   str(directory)], env=dict(os.environ, PKEXEC_UID='0'),
                                  capture_output=True, text=True, timeout=15)

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            legacy()
            result = migrate(directory) # missing sources AFTER privileged revocation
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(installed.read_bytes(), b'')
            self.assertEqual(installed.stat().st_mode & 0o111, 0)
            self.assertFalse(rule.exists())

            for name in ('ngpost-vpn-helper.sh', 'ngpost-vpn-uninstall.sh'):
                shutil.copyfile(source / 'scripts' / name, directory / name)
            shutil.copyfile(source / 'polkit/49-ngpost-vpn.rules.in', directory / '49-ngpost-vpn.rules.in')
            legacy()
            result = migrate(directory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('readonly NGPOST_VPN_HELPER_SECURITY_REVISION=3', installed.read_text())
            self.assertEqual(installed.stat().st_uid, 0)
            self.assertEqual(installed.stat().st_mode & 0o777, 0o755)
            self.assertTrue(rule.exists())
            self.assertNotIn('@@USER@@', rule.read_text())


if __name__ == '__main__':
    unittest.main(verbosity=2)
