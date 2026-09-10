"""Publish deterministic SHA-256 hashes for release artifacts; no signing keys."""
import hashlib
import json
import os
from pathlib import Path
import re

root = Path('artifacts')
tag = os.environ['RELEASE_TAG']
if not re.fullmatch(r'v?[0-9][0-9A-Za-z._-]*', tag):
    raise SystemExit('Invalid release tag')
assets = []
for path in sorted(root.iterdir()):
    if path.is_symlink() or not path.is_file():
        raise SystemExit('Expected regular release file: ' + path.name)
    # Idempotent: do not hash our own previous outputs. Stale signatures must
    # not accidentally accompany this unsigned release.
    if path.name in ('manifest.json', 'SHA256SUMS'):
        continue
    if path.name.endswith('.sig'):
        raise SystemExit('Unexpected signature asset: ' + path.name)
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', path.name):
        raise SystemExit('Unsafe checksum filename: ' + path.name)
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(65536), b''):
            digest.update(block)
    assets.append({'name': path.name, 'size': path.stat().st_size, 'sha256': digest.hexdigest()})
if not assets:
    raise SystemExit('No release artifacts')
(root / 'manifest.json').write_text(json.dumps({'schema': 1, 'tag': tag, 'assets': assets}, sort_keys=True) + '\n')
(root / 'SHA256SUMS').write_text(''.join(a['sha256'] + '  ' + a['name'] + '\n' for a in assets))
print('Generated manifest.json and SHA256SUMS for', len(assets), 'artifacts (unsigned).')
