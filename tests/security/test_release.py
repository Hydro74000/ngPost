import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / '.github/scripts/sign-release.py'
CHECK = SCRIPT.with_name('check-release-key.py')


class ReleaseSigningTests(unittest.TestCase):
    def test_key_provisioning_check(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            private, public = root / 'private.pem', root / 'public.pem'
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt',
                            'rsa_keygen_bits:3072', '-out', str(private)], check=True,
                           capture_output=True)
            subprocess.run(['openssl', 'pkey', '-in', str(private), '-pubout',
                            '-out', str(public)], check=True, capture_output=True)
            command = ['python3', str(CHECK), '--public', str(public)]
            subprocess.run(command, check=True, capture_output=True)
            env = dict(os.environ, RELEASE_SIGNING_KEY='')
            self.assertNotEqual(subprocess.run(command + ['--require-private'], env=env,
                                capture_output=True).returncode, 0)
            env['RELEASE_SIGNING_KEY'] = private.read_text()
            result = subprocess.run(command + ['--require-private'], env=env,
                                    check=True, capture_output=True)
            self.assertNotIn(b'PRIVATE KEY', result.stdout + result.stderr)
            self.assertNotEqual(subprocess.run(['python3', str(CHECK), '--public', str(private)],
                                capture_output=True).returncode, 0)

    def test_signing_requires_matching_key_and_covers_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'artifacts').mkdir()
            (root / 'artifacts/test.zip').write_bytes(b'package')
            public = root / 'src/utils/update/update-key.pem'
            public.parent.mkdir(parents=True)
            private = root / 'private.pem'
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'RSA', '-pkeyopt',
                            'rsa_keygen_bits:2048', '-out', str(private)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(['openssl', 'pkey', '-in', str(private), '-pubout',
                            '-out', str(public)], check=True, stdout=subprocess.DEVNULL)
            env = dict(os.environ, RELEASE_TAG='v-test', RELEASE_SIGNING_KEY='')
            command = ['python3', str(SCRIPT)]
            self.assertNotEqual(subprocess.run(command, cwd=root, env=env,
                                capture_output=True).returncode, 0)
            # The failed run wrote unsigned manifests; don't treat these as input assets.
            for name in ('manifest.json', 'SHA256SUMS'):
                (root / 'artifacts' / name).unlink()
            env['RELEASE_SIGNING_KEY'] = private.read_text()
            subprocess.run(command, cwd=root, env=env, check=True, capture_output=True)
            manifest = json.loads((root / 'artifacts/manifest.json').read_text())
            self.assertEqual([a['name'] for a in manifest['assets']], ['test.zip'])
            for name in ('manifest.json', 'SHA256SUMS'):
                verify = ['openssl', 'dgst', '-sha256', '-verify', str(public),
                          '-signature', str(root / 'artifacts' / (name + '.sig')),
                          str(root / 'artifacts' / name)]
                subprocess.run(verify, check=True, capture_output=True)
                (root / 'artifacts' / name).write_bytes(b'tampered')
                self.assertNotEqual(subprocess.run(verify, capture_output=True).returncode, 0)
            public.write_text('invalid key')
            self.assertNotEqual(subprocess.run(command, cwd=root, env=env,
                                capture_output=True).returncode, 0)
