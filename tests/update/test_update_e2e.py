"""Execute the shipped installer as separate processes on each native OS.

NGPOST_UPDATE_FIXTURE is the native fixture built from update_fixture.cpp.
No mocks of hashing, extraction, process waiting, replacement or rollback.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[2]
INSTALLER = REPO / 'src/utils/update/install_update.py'


def executable(root):
    return root / ('Contents/MacOS/ngPost' if sys.platform == 'darwin'
                   else 'ngPost.exe' if os.name == 'nt' else 'ngPost')


def wait_file(path, process, timeout=30):
    end = time.monotonic() + timeout
    while not path.exists():
        if process.poll() is not None:
            error = path.parent / 'error.txt'
            detail = error.read_text() if error.exists() else 'no error journal'
            raise AssertionError(f'process exited {process.returncode} before {path.name}: {detail}')
        if time.monotonic() > end:
            raise AssertionError(f'timed out waiting for {path}')
        time.sleep(0.05)


def make_archive(root, fixture, failure=None):
    suffix = 'macos.zip' if sys.platform == 'darwin' else 'windows-x86_64.zip' if os.name == 'nt' else 'linux-x86_64.tar.gz'
    name = 'ngPost-v99.0-' + suffix
    folder = 'ngPost.app' if sys.platform == 'darwin' else name.removesuffix('.zip').removesuffix('.tar.gz')
    package = root / 'package' / folder
    binary = executable(package)
    binary.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(fixture, binary)
    (package / '.ngpost-installation').touch()
    (package / 'generation').write_text('new')
    if failure:
        (package / failure).touch()
    archive = root / name
    if name.endswith('.zip'):
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as output:
            for path in package.rglob('*'):
                output.write(path, path.relative_to(package.parent).as_posix())
    else:
        with tarfile.open(archive, 'w:gz') as output:
            output.add(package, arcname=folder)
    return archive


def stage(work, archive, tag='v99.0'):
    work.mkdir()
    shutil.copy2(archive, work / 'archive')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (work / 'manifest.json').write_text(json.dumps({'schema': 1, 'tag': tag, 'assets': [
        {'name': archive.name, 'size': archive.stat().st_size, 'sha256': digest}]}))


def run_phase(script, phase, work, *args):
    return subprocess.run([sys.executable, '-I', str(script), phase, str(work), *map(str, args)],
                          capture_output=True, text=True, timeout=60)


class NativeUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ngpost update ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.install = self.root / 'installed'
        self.work = self.root / 'transaction'
        fixture = Path(os.environ['NGPOST_UPDATE_FIXTURE']).resolve()
        self.fixture = fixture
        binary = executable(self.install)
        binary.parent.mkdir(parents=True)
        shutil.copy2(fixture, binary)
        (self.install / '.ngpost-installation').touch()
        (self.install / 'generation').write_text('old')

    def prepare(self, failure=None):
        archive = make_archive(self.root, self.fixture, failure)
        stage(self.work, archive)
        return run_phase(INSTALLER, 'prepare', self.work, '--tag', 'v99.0',
                         '--asset', archive.name, '--install', self.install)

    def commit(self, cancel=False):
        generation = (self.install / 'generation').read_text()
        with subprocess.Popen([str(executable(self.install)), '--hold']) as old:
            try:
                with subprocess.Popen([sys.executable, '-I', str(INSTALLER), 'commit',
                                       str(self.work), '--pid', str(old.pid)],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE) as installer:
                    try:
                        wait_file(self.work / 'ready', installer)
                        time.sleep(0.2)
                        self.assertIsNone(installer.poll(), 'must wait for the old executable to exit')
                        self.assertEqual((self.install / 'generation').read_text(), generation)
                        if cancel:
                            (self.work / 'cancelled').touch()
                        (self.install / 'exit').touch()
                        old.wait(timeout=10)  # Reap before the POSIX kill(pid, 0) check.
                        output = installer.communicate(timeout=30)
                        return installer.returncode, output
                    finally:
                        if installer.poll() is None:
                            installer.kill()
            finally:
                if old.poll() is None:
                    old.kill()

    def test_replace_restart_and_second_update(self):
        self.assertEqual(self.prepare().returncode, 0)
        code, output = self.commit()
        self.assertEqual(code, 0, output)
        self.assertTrue((self.work / 'complete').is_file())
        self.assertEqual((self.install / 'generation').read_text(), 'new')
        for _ in range(100):
            if (self.install / 'restarted').exists():
                break
            time.sleep(0.05)
        self.assertTrue((self.install / 'restarted').is_file())
        # The installation remains owned and can be updated again.
        previous = json.loads((self.work / 'transaction.json').read_text())
        backup = Path(previous['backup'] if os.name == 'nt' else previous['candidate'])
        self.assertEqual((backup / 'generation').read_text(), 'old')
        self.work = self.root / 'second transaction'
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.commit()[0], 0)

    def test_cancel_keeps_old_installation(self):
        self.assertEqual(self.prepare().returncode, 0)
        self.assertNotEqual(self.commit(cancel=True)[0], 0)
        self.assertEqual((self.install / 'generation').read_text(), 'old')
        self.assertFalse((self.work / 'complete').exists())

    def test_failed_probe_after_swap_rolls_back(self):
        self.assertEqual(self.prepare('fail-after-swap').returncode, 0)
        self.assertNotEqual(self.commit()[0], 0)
        self.assertEqual((self.install / 'generation').read_text(), 'old')
        self.assertFalse((self.install / 'restarted').exists())

    def test_broken_candidate_never_reaches_handoff(self):
        self.assertNotEqual(self.prepare('fail-probe').returncode, 0)
        self.assertFalse((self.work / 'prepared.json').exists())
        self.assertEqual((self.install / 'generation').read_text(), 'old')

    def test_corrupt_archive_never_reaches_handoff(self):
        archive = make_archive(self.root, self.fixture)
        stage(self.work, archive)
        with (self.work / 'archive').open('r+b') as output:
            output.write(b'corrupt')
        result = run_phase(INSTALLER, 'prepare', self.work, '--tag', 'v99.0',
                           '--asset', archive.name, '--install', self.install)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SHA-256', result.stderr)
        self.assertFalse((self.work / 'unpacked').exists())


if __name__ == '__main__':
    unittest.main()
