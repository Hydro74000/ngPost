"""Generate signed manifests for the exact release artifacts after all gates."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

root = Path('artifacts')
assets = []
for path in sorted(root.iterdir()):
    if path.is_file():
        digest = hashlib.sha256()
        with path.open('rb') as source:
            for block in iter(lambda: source.read(65536), b''):
                digest.update(block)
        assets.append({'name': path.name, 'size': path.stat().st_size, 'sha256': digest.hexdigest()})
(root / 'manifest.json').write_text(json.dumps({'schema': 1, 'tag': os.environ['RELEASE_TAG'], 'assets': assets}, sort_keys=True) + '\n')
(root / 'SHA256SUMS').write_text(''.join(a['sha256'] + '  ' + a['name'] + '\n' for a in assets))
key = os.environ.get('RELEASE_SIGNING_KEY', '')
if not key:
    raise SystemExit('RELEASE_SIGNING_KEY is required for publication')
with tempfile.TemporaryDirectory() as tmp:
    private = Path(tmp) / 'key.pem'
    private.write_text(key)
    private.chmod(0o600)
    for name in ('manifest.json', 'SHA256SUMS'):
        subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(private), '-out', str(root / (name + '.sig')), str(root / name)], check=True)
        # A mismatched repository key must stop publication, not strand clients.
        subprocess.run(['openssl', 'dgst', '-sha256', '-verify', 'src/utils/update/update-key.pem', '-signature', str(root / (name + '.sig')), str(root / name)], check=True)
