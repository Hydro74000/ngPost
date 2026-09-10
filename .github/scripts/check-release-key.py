"""Validate the public update key, optionally proving the CI secret matches it.

Never print private key material or place it in the source/artifact directories.
"""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


def check(public, require_private=False):
    encoded = public.read_text(encoding='ascii')
    if 'PRIVATE KEY' in encoded:
        raise ValueError('A private key must never be committed as the update key')
    der = base64.b64decode(''.join(line for line in encoded.splitlines()
                                 if not line.startswith('-----')), validate=True)
    # OpenSSL validates the ASN.1 and enforces the supported algorithm/size.
    result = subprocess.run(['openssl', 'pkey', '-pubin', '-in', str(public),
                             '-text', '-noout'], capture_output=True, text=True, check=True)
    import re
    bits = re.search(r'Public-Key: \((\d+) bit\)', result.stdout)
    if not bits or int(bits[1]) < 3072 or 'Exponent:' not in result.stdout:
        raise ValueError('An RSA public key of at least 3072 bits is required')
    print('Public update key SHA-256 (SPKI DER): ' + hashlib.sha256(der).hexdigest())
    if not require_private:
        return
    private = os.environ.get('RELEASE_SIGNING_KEY', '')
    if not private:
        raise ValueError('RELEASE_SIGNING_KEY is missing from the protected environment')
    with tempfile.TemporaryDirectory(prefix='ngpost-key-check-') as directory:
        root = Path(directory)
        key = root / 'private.pem'
        with open(key, 'x', opener=lambda path, flags: os.open(path, flags, 0o600)) as output:
            output.write(private)
        proof = root / 'proof'
        proof.write_bytes(b'ngPost signing-key provisioning proof; NOT a release manifest\n' + os.urandom(32))
        signature = root / 'signature'
        subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(key), '-out',
                        str(signature), str(proof)], check=True, capture_output=True)
        subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(public), '-signature',
                        str(signature), str(proof)], check=True, capture_output=True)
    print('Protected CI signing key matches the embedded public key.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--public', type=Path, default=Path('src/utils/update/update-key.pem'))
    parser.add_argument('--require-private', action='store_true')
    args = parser.parse_args()
    try:
        check(args.public, args.require_private)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit('Signing-key check failed: ' + str(error))
