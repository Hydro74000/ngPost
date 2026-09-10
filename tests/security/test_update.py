import importlib.util
import io
import json
from pathlib import Path
import tempfile
import tarfile
import unittest
import zipfile
from unittest import mock

spec = importlib.util.spec_from_file_location('updater', Path(__file__).resolve().parents[2] / 'src/utils/update/install_update.py')
updater = importlib.util.module_from_spec(spec)
spec.loader.exec_module(updater)


class UpdateTests(unittest.TestCase):
    def test_archive_metadata_limits_precede_library_allocations(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            archive = root / 'a.zip'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('file', b'ok')
            with mock.patch.object(updater, 'MAX_ZIP_DIRECTORY', 1), self.assertRaises(ValueError):
                updater.extract(archive, root / 'zip-out')
            with tarfile.open(root / 'a.tar.gz', 'w:gz') as tar:
                info = tarfile.TarInfo('file')
                info.pax_headers = {'comment': 'X' * 65537}
                tar.addfile(info)
            with self.assertRaises(ValueError):
                updater.extract(root / 'a.tar.gz', root / 'tar-out')

    def test_hostile_paths(self):
        for name in ('../escape', '/absolute', 'C:/escape', 'a\\..\\b', 'NUL', 'foo.', 'x\0y'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                updater.safe_name(name)

    def test_zip_traversal_and_duplicates(self):
        for names in (('../escape',), ('A', 'a')):
            with tempfile.TemporaryDirectory() as d:
                root = Path(d)
                with zipfile.ZipFile(root / 'a.zip', 'w') as z:
                    for name in names:
                        z.writestr(name, b'content')
                with self.assertRaises(ValueError):
                    updater.extract(root / 'a.zip', root / 'out')
                self.assertFalse((root / 'escape').exists())

    def test_tar_links_and_expansion(self):
        for target in ('../../escape', '/etc/passwd'):
            with tempfile.TemporaryDirectory() as d:
                root = Path(d)
                with tarfile.open(root / 'a.tar.gz', 'w:gz') as tar:
                    link = tarfile.TarInfo('pkg/link')
                    link.type, link.linkname = tarfile.SYMTYPE, target
                    tar.addfile(link)
                with self.assertRaises(ValueError):
                    updater.extract(root / 'a.tar.gz', root / 'out')
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            with zipfile.ZipFile(root / 'a.zip', 'w') as z:
                z.writestr('pkg/file', b'12345')
            with mock.patch.object(updater, 'MAX_BYTES', 4), self.assertRaises(ValueError):
                updater.extract(root / 'a.zip', root / 'out')

    def test_checksum_manifest_and_tampering(self):
        import hashlib
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            data = b'archive content'
            (root / 'archive').write_bytes(data)
            (root / 'manifest.json').write_text(json.dumps({'schema': 1, 'tag': 'v1.0', 'assets': [{'name': 'a.zip', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}]}))
            updater.verify(root, 'v1.0', 'a.zip')
            with self.assertRaises(ValueError):
                updater.verify(root, 'v2.0', 'a.zip')
            (root / 'archive').write_bytes(b'X' * len(data))
            with self.assertRaises(ValueError):
                updater.verify(root, 'v1.0', 'a.zip')
            (root / 'manifest.json').write_text('{}')
            with self.assertRaises(ValueError):
                updater.verify(root, 'v1.0', 'a.zip')

    def test_malformed_or_missing_checksum_blocks_before_extraction(self):
        import hashlib
        record = {'name': 'a.zip', 'size': 1, 'sha256': hashlib.sha256(b'x').hexdigest()}
        bad_assets = [[], [record, record], [dict(record, sha256='x')],
                      [dict(record, sha256=None)], [dict(record, size=True)],
                      [dict(record, size=-1)], [dict(record, size=1024**3 + 1)],
                      ['not a record']]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'archive').write_bytes(b'x')
            with self.assertRaises(FileNotFoundError):
                updater.verify(root, 'v1', 'a.zip')
            for assets in bad_assets:
                (root / 'manifest.json').write_text(json.dumps({'schema': 1, 'tag': 'v1', 'assets': assets}))
                with self.subTest(assets=assets), mock.patch.object(updater, 'extract') as extract:
                    with self.assertRaises(ValueError):
                        updater.prepare(root, 'v1', 'a.zip', root / 'installation')
                    extract.assert_not_called()
            for malformed in ('[]', '{"schema":1,"schema":1}', 'x' * (1024**2 + 1)):
                (root / 'manifest.json').write_text(malformed)
                with self.assertRaises(ValueError):
                    updater.verify(root, 'v1', 'a.zip')

    def test_windows_failed_second_rename_restores_previous(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            install, candidate, backup = root / 'install', root / 'new', root / 'old'
            install.mkdir(); candidate.mkdir()
            (install / 'version').write_text('old')
            (candidate / 'version').write_text('new')
            rename = updater.os.rename
            def fail_second(source, destination):
                if source == candidate:
                    raise PermissionError('simulated Windows rename failure')
                return rename(source, destination)
            with mock.patch.object(updater.os, 'name', 'nt'), \
                 mock.patch.object(updater.os, 'rename', side_effect=fail_second), \
                 self.assertRaises(PermissionError):
                updater.replace(candidate, install, backup)
            self.assertEqual((install / 'version').read_text(), 'old')
            self.assertEqual((candidate / 'version').read_text(), 'new')
            self.assertFalse(backup.exists())

    def test_windows_swap_and_explicit_rollback(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            install, candidate, backup = root / 'install', root / 'new', root / 'old'
            install.mkdir(); candidate.mkdir()
            (install / 'version').write_text('old')
            (candidate / 'version').write_text('new')
            with mock.patch.object(updater.os, 'name', 'nt'):
                updater.replace(candidate, install, backup)
                self.assertEqual((install / 'version').read_text(), 'new')
                updater.rollback(candidate, install, backup)
            self.assertEqual((install / 'version').read_text(), 'old')

    def test_macos_exchange_requests_atomic_swap(self):
        library = mock.Mock()
        library.renamex_np.return_value = 0
        with mock.patch.object(updater.sys, 'platform', 'darwin'), \
             mock.patch.object(updater.ctypes, 'CDLL', return_value=library):
            updater.exchange('/old', '/new')
        library.renamex_np.assert_called_once_with(b'/old', b'/new', 2)

    @unittest.skipUnless(__import__('sys').platform.startswith('linux'), 'renameat2')
    def test_atomic_exchange_and_rollback(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            old, new = root / 'installed', root / 'candidate'
            old.mkdir(); new.mkdir()
            (old / 'version').write_text('old')
            (new / 'version').write_text('new')
            updater.replace(new, old, root / 'backup')
            self.assertEqual((old / 'version').read_text(), 'new')
            updater.rollback(new, old, root / 'backup')
            self.assertEqual((old / 'version').read_text(), 'old')

    @unittest.skipUnless(__import__('sys').platform.startswith('linux'), 'renameat2')
    def test_commit_failure_restores_previous_and_cancel_does_not_swap(self):
        for canceled in (False, True):
            with tempfile.TemporaryDirectory() as d:
                root = Path(d)
                install, work = root / 'install', root / 'work'
                candidate = work / 'new'
                install.mkdir(); candidate.mkdir(parents=True)
                (install / 'version').write_text('old')
                (candidate / 'version').write_text('new')
                (work / 'prepared.json').write_text(json.dumps({'candidate': str(candidate), 'install': str(install)}))
                if canceled:
                    (work / 'cancelled').touch()
                with mock.patch.object(updater.os, 'kill', side_effect=ProcessLookupError), \
                     mock.patch.object(updater, 'probe', side_effect=ValueError('failed launch')), \
                     self.assertRaises(ValueError):
                    updater.commit(work, 123)
                self.assertEqual((install / 'version').read_text(), 'old')
                self.assertEqual((candidate / 'version').read_text(), 'new')


if __name__ == '__main__':
    unittest.main()
