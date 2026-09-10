"""SHA-256-checked updates: bounded extraction, directory swap, rollback. No shell.

The manifest comes from the same HTTPS GitHub release as the archive. Hashes
check integrity against that manifest, NOT independent publisher authenticity.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import subprocess
import sys
import tarfile
import time
import zipfile

MAX_BYTES = 2 * 1024**3
MAX_ENTRIES = 50000
MAX_ZIP_DIRECTORY = 16 * 1024**2


def bounded_zip_metadata(archive):
    # ZipFile materializes the complete central directory in RAM before we
    # can count entries. Validate its advertised size FIRST. Packages are
    # capped at 1 GiB / 50k entries and have no reason to require ZIP64.
    with Path(archive).open('rb') as source:
        source.seek(max(0, Path(archive).stat().st_size - 65557))
        tail = source.read(65557)
    offset = tail.rfind(b'PK\x05\x06')
    if offset < 0 or len(tail) - offset < 22:
        raise ValueError('invalid ZIP directory')
    end = struct.unpack('<4s4H2IH', tail[offset:offset + 22])
    if (end[1] or end[2] or end[3] != end[4] or end[4] > MAX_ENTRIES
            or end[5] > MAX_ZIP_DIRECTORY or end[6] == 0xffffffff
            or len(tail) - offset != 22 + end[7]
            or (offset >= 20 and tail[offset - 20:offset - 16] == b'PK\x06\x07')
            or end[6] + end[5] > Path(archive).stat().st_size - 22 - end[7]):
        raise ValueError('ZIP metadata exceeds supported limits')


class BoundedTarInfo(tarfile.TarInfo):
    def _proc_member(self, archive):
        # PAX and GNU name extensions are read whole by tarfile before the
        # caller sees a member. Bound those allocations at the header parser.
        if self.type in (tarfile.XHDTYPE, tarfile.XGLTYPE, tarfile.SOLARIS_XHDTYPE,
                         tarfile.GNUTYPE_LONGNAME, tarfile.GNUTYPE_LONGLINK) and self.size > 65536:
            raise ValueError('oversized TAR metadata')
        if self.type == tarfile.GNUTYPE_SPARSE:
            raise ValueError('sparse TAR entries are unsupported')
        return super()._proc_member(archive)

    def _proc_gnusparse_10(self, next_member, pax_headers, archive):
        raise ValueError('sparse TAR metadata is unsupported')


def safe_name(name):
    path = PurePosixPath(name)
    if not name or not path.parts or path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
        raise ValueError('archive path escapes destination')
    for part in path.parts:
        if part.endswith((' ', '.')) or any(ord(c) < 32 for c in part):
            raise ValueError('ambiguous archive path')
        if re.fullmatch(r'(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?', part):
            raise ValueError('reserved archive name')
    return path


def extract(archive, destination):
    destination = Path(destination).resolve()
    destination.mkdir(mode=0o700)
    links, names, total = [], set(), 0

    def member(name, size, mode, kind, source=None, target=None):
        nonlocal total
        relative = safe_name(name)
        key = str(relative).casefold()
        if key in names:
            raise ValueError('duplicate archive entry')
        names.add(key)
        total += size
        if size < 0 or total > MAX_BYTES or len(names) > MAX_ENTRIES:
            raise ValueError('archive expansion limit exceeded')
        path = destination.joinpath(*relative.parts)
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if kind == 'directory':
            path.mkdir(mode=0o700, exist_ok=True)
        elif kind == 'link':
            if not target or '\\' in target or ':' in target or Path(target).is_absolute():
                raise ValueError('invalid symbolic link')
            links.append((path, target))
        elif kind == 'file':
            remaining = size
            with path.open('xb') as out:
                while remaining:
                    data = source.read(min(65536, remaining))
                    if not data:
                        raise ValueError('truncated member')
                    out.write(data)
                    remaining -= len(data)
                if source.read(1):
                    raise ValueError('incorrect member size')
            path.chmod((mode & 0o755) | 0o600)
        else:
            raise ValueError('special files and hard links are forbidden')

    if zipfile.is_zipfile(archive):
        bounded_zip_metadata(archive)
        with zipfile.ZipFile(archive) as z:
            for e in z.infolist():
                mode = e.external_attr >> 16
                if stat.S_ISLNK(mode):
                    if e.file_size > 4096:
                        raise ValueError('oversized link')
                    member(e.filename, e.file_size, mode, 'link', target=z.read(e).decode('utf-8'))
                elif e.is_dir():
                    member(e.filename, 0, mode, 'directory')
                elif stat.S_IFMT(mode) not in (0, stat.S_IFREG):
                    raise ValueError('special zip member')
                else:
                    with z.open(e) as source:
                        member(e.filename, e.file_size, mode, 'file', source)
    else:
        with tarfile.open(archive, 'r|gz', tarinfo=BoundedTarInfo) as tar:
            for e in tar:
                kind = 'directory' if e.isdir() else 'link' if e.issym() else 'file' if e.isfile() else 'special'
                member(e.name, e.size, e.mode, kind, tar.extractfile(e) if e.isfile() else None, e.linkname)
    # No link is followed while extracting regular files. Check the complete
    # link graph before any package content can be executed.
    for path, target in links:
        path.symlink_to(target)
    for path, _ in links:
        if not path.resolve().is_relative_to(destination):
            raise ValueError('symlink escapes archive')


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON field')
        result[key] = value
    return result


def verify(work, tag, asset):
    manifest = work / 'manifest.json'
    with manifest.open('rb') as source:
        encoded = source.read(1024**2 + 1)
    if len(encoded) > 1024**2:
        raise ValueError('oversized checksum metadata')
    doc = json.loads(encoded, object_pairs_hook=unique_object)
    if not isinstance(doc, dict) or type(doc.get('schema')) is not int or doc['schema'] != 1 or doc.get('tag') != tag:
        raise ValueError('checksum manifest is malformed or for another release')
    assets = doc.get('assets')
    if not isinstance(assets, list) or len(assets) > MAX_ENTRIES or any(not isinstance(a, dict) for a in assets):
        raise ValueError('invalid asset list')
    matches = [a for a in assets if a.get('name') == asset]
    if len(matches) != 1:
        raise ValueError('asset absent or duplicated in checksum manifest')
    archive, expected = work / 'archive', matches[0]
    if (type(expected.get('size')) is not int or not 0 < expected['size'] <= 1024**3
            or not isinstance(expected.get('sha256'), str)
            or not re.fullmatch('[0-9a-f]{64}', expected['sha256'])):
        raise ValueError('invalid asset size or SHA-256 hash')
    if archive.stat().st_size != expected['size']:
        raise ValueError('archive size mismatch')
    digest = hashlib.sha256()
    with archive.open('rb') as source:
        for block in iter(lambda: source.read(65536), b''):
            digest.update(block)
    if digest.hexdigest() != expected['sha256']:
        raise ValueError('SHA-256 hash mismatch')


def executable(root):
    return root / ('Contents/MacOS/ngPost' if sys.platform == 'darwin' else 'ngPost.exe' if os.name == 'nt' else 'ngPost')


def probe(root):
    subprocess.run([str(executable(root)), '--version'], check=True, timeout=30,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))


def prepare(work, tag, asset, install):
    verify(work, tag, asset)
    destination = work / 'unpacked'
    extract(work / 'archive', destination)
    root_name = 'ngPost.app' if sys.platform == 'darwin' else asset.removesuffix('.tar.gz').removesuffix('.zip')
    candidate = destination / root_name
    if not executable(candidate).is_file() or executable(candidate).is_symlink() or candidate.is_symlink():
        raise ValueError('unexpected package layout')
    if not (candidate / '.ngpost-installation').is_file():
        raise ValueError('missing package ownership marker')
    probe(candidate)
    (work / 'prepared.json').write_text(json.dumps({'candidate': str(candidate), 'install': str(install)}))


def exchange(left, right):
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == 'darwin':
        rc = libc.renamex_np(os.fsencode(left), os.fsencode(right), 2)
    else:
        rc = libc.renameat2(-100, os.fsencode(left), -100, os.fsencode(right), 2)
    if rc:
        raise OSError(ctypes.get_errno(), 'atomic directory exchange failed')


def replace(candidate, install, backup):
    if os.name == 'nt':
        os.rename(install, backup)
        try:
            os.rename(candidate, install)
        except BaseException:
            os.rename(backup, install)
            raise
    else:
        exchange(install, candidate)


def rollback(candidate, install, backup):
    if os.name == 'nt':
        os.rename(install, candidate)
        os.rename(backup, install)
    else:
        exchange(install, candidate)


def commit(work, pid):
    doc = json.loads((work / 'prepared.json').read_text())
    candidate, install, backup = Path(doc['candidate']), Path(doc['install']), work / 'previous'
    if not candidate.resolve().is_relative_to(work.resolve()) or install.parent != work.parent:
        raise ValueError('invalid transaction paths')
    journal = work / 'transaction.json'
    journal.write_text(json.dumps({'install': str(install), 'candidate': str(candidate), 'backup': str(backup)}))
    with journal.open('rb') as state:
        os.fsync(state.fileno())
    if os.name == 'nt':
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        handle = kernel.OpenProcess(0x100000, False, pid)
        if not handle:
            raise OSError('cannot acquire application process handle')
        (work / 'ready').touch()
        try:
            if kernel.WaitForSingleObject(ctypes.c_void_p(handle), 120000) != 0:
                raise TimeoutError('application is still running')
        finally:
            kernel.CloseHandle(ctypes.c_void_p(handle))
    else:
        (work / 'ready').touch()
        for _ in range(1200):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.1)
        else:
            raise TimeoutError('application is still running')
    if (work / 'cancelled').exists():
        raise ValueError('update canceled')
    replace(candidate, install, backup)
    try:
        probe(install)
        subprocess.Popen([str(executable(install))], cwd=install, start_new_session=True)
    except BaseException:
        rollback(candidate, install, backup)
        raise
    (work / 'complete').touch()
    # Keep the previous installation and journal for recovery, never delete it.


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('phase', choices=['prepare', 'commit'])
    parser.add_argument('work', type=Path)
    for option in ('tag', 'asset', 'install'):
        parser.add_argument('--' + option)
    parser.add_argument('--pid', type=int)
    args = parser.parse_args()
    try:
        if args.phase == 'prepare':
            prepare(args.work, args.tag, args.asset, Path(args.install))
        else:
            commit(args.work, args.pid)
    except Exception as error:
        (args.work / 'error.txt').write_text(str(error))
        print(str(error), file=sys.stderr)
        sys.exit(1)
