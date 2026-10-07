"""Execute the Linux profile sanitizer without starting the privileged helper."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[2] / 'src/vpn/scripts/ngpost-vpn-helper.sh'


@unittest.skipUnless(os.name == 'posix' and shutil.which('bash'), 'requires POSIX bash')
class WireGuardHelperTests(unittest.TestCase):
    def sanitize(self, profile):
        source = HELPER.read_text()
        start = source.index('WG_INTERFACE_KEYS="')
        end = source.index('\nmkdir -p "$PRIVATE_DIR"', start)
        # Load the actual key tables and function, stopping before runtime setup.
        script = ('set -eu\nterminal_error() { printf "%s\\n" "$*" >&2; }\n'
                  + source[start:end] + '\nsanitize_wireguard_profile "$1"\n')
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'profile.conf'
            path.write_text(profile)
            result = subprocess.run(['bash', '-c', script, 'sanitizer-test', str(path)],
                                    capture_output=True, text=True)
            return result, path.read_text()

    def test_inline_comments_are_removed_from_sections_and_values(self):
        result, content = self.sanitize(
            '# comment\n[Interface] # interface = ignored\n'
            'PrivateKey = private # PostUp = ignored\n'
            '[Peer] # peer\nPublicKey = public # comment\n')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(content, '[Interface]\nPrivateKey = private\n[Peer]\nPublicKey = public\n')

    def test_inline_comments_do_not_hide_unreviewed_keys(self):
        profile = '[Interface] # comment\nNeverReviewed = secret # comment\n'
        result, content = self.sanitize(profile)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unreviewed-key withheld line 2', result.stderr)
        self.assertNotIn('secret', result.stderr)
        self.assertEqual(content, profile)
