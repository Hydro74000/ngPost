"""Run the unmodified 5.5.1 C++ updater against a candidate release archive.

The candidate is presented as a future stable release, including when CI builds
an unstable artifact. Bytes inside the archive are unchanged. Run sequentially
on disposable runners: the old client uses fixed /tmp paths on Unix.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from check_release_package import check_version, stop
from test_update_e2e import executable


def running(binary):
    if os.name == 'nt':
        command = ['powershell', '-NoProfile', '-Command',
                   'Get-CimInstance Win32_Process | Where-Object { '
                   '$_.ExecutablePath -eq $env:NGPOST_TEST_BINARY } | Select-Object -ExpandProperty ProcessId']
        output = subprocess.check_output(command, env=dict(os.environ, NGPOST_TEST_BINARY=str(binary)))
        return [int(pid) for pid in output.split()]
    output = subprocess.check_output(['ps', '-axo', 'pid=,args='], text=True)
    return [int(line.strip().split(None, 1)[0]) for line in output.splitlines()
            if len(line.strip().split(None, 1)) == 2
            and (line.strip().split(None, 1)[1] == str(binary)
                 or line.strip().split(None, 1)[1].startswith(str(binary) + ' '))]


def check(archive, client):
    if os.name != 'nt':
        for fixed in ('/tmp/ngPost-update', '/tmp/ngPost-update.sh'):
            if Path(fixed).exists():
                raise RuntimeError(f'legacy test refuses to overwrite existing {fixed}')
    with tempfile.TemporaryDirectory(prefix='ngpost-legacy-') as temporary:
        root = Path(temporary).resolve()
        install = root / ('ngPost.app' if sys.platform == 'darwin' else 'installed')
        binary = executable(install)
        binary.parent.mkdir(parents=True)
        shutil.copy2(client, binary)
        old_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
        suffix = 'macos.zip' if sys.platform == 'darwin' else 'windows-x86_64.zip' if os.name == 'nt' else 'linux-x86_64.tar.gz'
        name = 'ngPost-v99.0-' + suffix
        url = 'https://github.com/Hydro74000/ngPost/releases/download/v99.0/' + name
        release = root / 'release.json'
        release.write_text(json.dumps({'tag_name': 'v99.0', 'assets': [
            {'name': name, 'size': archive.stat().st_size, 'browser_download_url': url}]}))
        scenario = root / 'scenario.json'
        scenario.write_text(json.dumps({url: str(archive),
            'https://api.github.com/repos/Hydro74000/ngPost/releases/latest': str(release)}))
        profile = root / 'profile'
        profile.mkdir()
        env = dict(os.environ, NGPOST_UPDATE_SCENARIO=str(scenario), QT_QPA_PLATFORM='offscreen',
                   HOME=str(profile), XDG_CONFIG_HOME=str(profile), LOCALAPPDATA=str(profile), APPDATA=str(profile),
                   TMPDIR=str(root), TEMP=str(root), TMP=str(root))
        try:
            with (root / 'legacy.log').open('wb') as log:
                result = subprocess.run([str(binary)], env=env, stdout=log, stderr=log, timeout=40)
            assert result.returncode == 0, (root / 'legacy.log').read_text()
            for _ in range(120):
                if (install / '.ngpost-installation').exists() and binary.exists():
                    try:
                        replaced = hashlib.sha256(binary.read_bytes()).hexdigest() != old_hash
                        if replaced and running(binary):
                            break
                    except PermissionError:
                        pass  # Windows copy may still hold the file.
                time.sleep(0.5)
            else:
                raise AssertionError('legacy updater failed to install and restart the candidate')
            print(check_version(binary, env))
            assert (install / '.ngpost-installation').is_file()
            print(f'PASS: 5.5.1 notification, download, legacy install and restart -> {archive.name}')
        finally:
            for pid in running(binary):
                stop(pid)
            time.sleep(1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--client', type=Path, required=True)
    args = parser.parse_args()
    check(args.archive.resolve(), args.client.resolve())
