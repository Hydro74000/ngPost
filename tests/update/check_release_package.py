"""Gate the actual release archive through preparation, swap and GUI restart.

Runs only in a disposable directory. A second preparation checks that the new
installation retains the ownership marker required for subsequent updates.
An optional previous installer validates compatibility with already shipped
SHA-256 clients, whose embedded verifier cannot be patched by a new release.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from test_update_e2e import INSTALLER, executable, run_phase, stage, wait_file


def stop(pid):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(pid)], capture_output=True, check=False)
    else:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass


def check_version(binary, env):
    result = subprocess.run([str(binary), '--version'], env=env, capture_output=True, timeout=30)
    output = result.stdout + result.stderr
    if result.returncode or b'SSL support: yes' not in output:
        raise AssertionError(f'packaged executable or TLS failed: {output!r}')
    return output.decode(errors='replace')


def check_package(archive, tag, previous_installer=None):
    with tempfile.TemporaryDirectory(prefix='ngpost release update ') as temporary:
        root = Path(temporary).resolve()
        install = root / ('ngPost.app' if sys.platform == 'darwin' else 'installed')
        work = root / 'transaction'
        stage(work, archive, tag)
        # Use the production manifest generator, not an independently invented schema.
        artifacts = root / 'artifacts'
        artifacts.mkdir()
        shutil.copy2(archive, artifacts / archive.name)
        generator = INSTALLER.parents[3] / '.github/scripts/release-checksums.py'
        subprocess.run([sys.executable, str(generator)], cwd=root,
                       env=dict(os.environ, RELEASE_TAG=tag), check=True)
        shutil.copy2(artifacts / 'manifest.json', work / 'manifest.json')
        result = run_phase(previous_installer or INSTALLER, 'prepare', work, '--tag', tag,
                           '--asset', archive.name, '--install', install)
        if result.returncode:
            raise AssertionError(result.stderr)
        candidate = Path(json.loads((work / 'prepared.json').read_text())['candidate'])
        expected = hashlib.sha256(executable(candidate).read_bytes()).hexdigest()
        shutil.copytree(candidate, install, symlinks=True)
        (install / 'previous-installation-sentinel').touch()
        profile = root / 'profile'
        profile.mkdir()
        config = profile / 'ngPost.conf'
        config.write_text('CHECK_FOR_UPDATES = false\n')
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', HOME=str(profile),
                   XDG_CONFIG_HOME=str(profile), APPDATA=str(profile), LOCALAPPDATA=str(profile))
        print(check_version(executable(install), env))
        # Real ngPost stays open until handoff acknowledges it is waiting.
        with subprocess.Popen([str(executable(install))], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as old:
            try:
                with (work / 'commit.log').open('wb') as log, subprocess.Popen(
                        [sys.executable, '-I', str(INSTALLER), 'commit', str(work),
                                       '--pid', str(old.pid)], env=env,
                                      stdout=log, stderr=log) as installer:
                    try:
                        wait_file(work / 'ready', installer)
                        if old.poll() is not None:
                            raise AssertionError('packaged GUI failed to stay running')
                        if not (install / 'previous-installation-sentinel').exists():
                            raise AssertionError('replaced before the old application exited')
                        old.terminate()
                        old.wait(timeout=15)
                        installer.wait(timeout=60)
                        if installer.returncode:
                            raise AssertionError((work / 'commit.log').read_text())
                    finally:
                        if installer.poll() is None:
                            installer.kill()
            finally:
                if old.poll() is None:
                    old.kill()
        launched = json.loads((work / 'launched.json').read_text())['pid']
        try:
            assert (work / 'complete').exists()
            assert not (install / 'previous-installation-sentinel').exists()
            assert hashlib.sha256(executable(install).read_bytes()).hexdigest() == expected
            print(check_version(executable(install), env))
            time.sleep(2)
            # A crash just after Popen must fail the gate too.
            if os.name == 'nt':
                running = subprocess.check_output(['tasklist', '/FI', f'PID eq {launched}', '/FO', 'CSV'])
                assert str(launched).encode() in running, 'restarted GUI exited'
            else:
                os.kill(launched, 0)
            second = root / 'next-update'
            stage(second, archive, tag)
            result = run_phase(INSTALLER, 'prepare', second, '--tag', tag,
                               '--asset', archive.name, '--install', install)
            assert result.returncode == 0, result.stderr
            assert config.read_text() == 'CHECK_FOR_UPDATES = false\n'
            print(f'PASS: {archive.name}: prepare, wait, swap, TLS, GUI restart, next update')
        finally:
            stop(launched)
            time.sleep(0.5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--previous-installer', type=Path)
    args = parser.parse_args()
    check_package(args.archive.resolve(), args.tag, args.previous_installer)
