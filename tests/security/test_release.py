import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / '.github/scripts/release-checksums.py'


class ReleaseChecksumTests(unittest.TestCase):
    def test_hashes_cover_every_artifact_without_keys_and_are_repeatable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifacts = root / 'artifacts'
            artifacts.mkdir()
            names = ['ngPost-v5.6-linux.tar.gz', 'ngPost-v5.6-windows.zip',
                     'ngPost-v5.6-setup.exe', 'ngPost-v5.6-macos.zip',
                     'ngPost-v5.6.AppImage', 'ngPost-v5.6.AppImage.zsync', 'ngPost.spdx.json']
            for name in names:
                (artifacts / name).write_bytes(name.encode())
            env = {k: v for k, v in os.environ.items() if not any(x in k for x in ('SIGNING', 'APPLE_'))}
            env['RELEASE_TAG'] = 'v5.6'
            command = ['python3', str(SCRIPT)]
            subprocess.run(command, cwd=root, env=env, check=True, capture_output=True)
            manifest = json.loads((artifacts / 'manifest.json').read_text())
            self.assertEqual(manifest['tag'], 'v5.6')
            self.assertEqual([a['name'] for a in manifest['assets']], sorted(names))
            expected_lines = []
            for asset in manifest['assets']:
                data = (artifacts / asset['name']).read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                self.assertEqual(asset['sha256'], digest)
                self.assertEqual(asset['size'], len(data))
                expected_lines.append(digest + '  ' + asset['name'] + '\n')
            sums = (artifacts / 'SHA256SUMS').read_text()
            self.assertEqual(sums, ''.join(expected_lines))
            subprocess.run(command, cwd=root, env=env, check=True, capture_output=True)
            self.assertEqual(json.loads((artifacts / 'manifest.json').read_text()), manifest)
            self.assertEqual((artifacts / 'SHA256SUMS').read_text(), sums)
            self.assertFalse(list(artifacts.glob('*.sig')))

    def test_invalid_assets_and_empty_release_are_rejected(self):
        for name in (None, 'bad\nname.zip', 'manifest.json.sig'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / 'artifacts').mkdir()
                if name:
                    (root / 'artifacts' / name).write_bytes(b'bad')
                result = subprocess.run(['python3', str(SCRIPT)], cwd=root,
                                        env=dict(os.environ, RELEASE_TAG='v5.6'), capture_output=True)
                self.assertNotEqual(result.returncode, 0)

    def test_output_symlinks_are_rejected_without_overwriting_target(self):
        for name in ('manifest.json', 'SHA256SUMS'):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                artifacts = root / 'artifacts'
                artifacts.mkdir()
                target = root / 'keep.txt'
                target.write_bytes(b'keep')
                (artifacts / 'app.zip').write_bytes(b'archive')
                (artifacts / name).symlink_to(target)
                result = subprocess.run(['python3', str(SCRIPT)], cwd=root,
                                        env=dict(os.environ, RELEASE_TAG='v5.6'), capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(target.read_bytes(), b'keep')
