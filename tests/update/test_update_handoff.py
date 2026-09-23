"""C++ release notification -> downloads -> embedded Python -> exit -> restart."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

from test_update_e2e import executable, make_archive, stage


class HandoffTests(unittest.TestCase):
    def test_cpp_handoff_replaces_the_running_client(self):
        with tempfile.TemporaryDirectory(prefix='ngpost handoff ') as temporary:
            root = Path(temporary).resolve()
            install = root / 'installed'
            client = executable(install)
            client.parent.mkdir(parents=True)
            shutil.copy2(os.environ['NGPOST_UPDATE_CLIENT'], client)
            (install / '.ngpost-installation').touch()
            archive = make_archive(root, os.environ['NGPOST_UPDATE_FIXTURE'])
            stage(root / 'server', archive)
            url = 'https://github.com/Hydro74000/ngPost/releases/download/v99.0/'
            release = root / 'release.json'
            release.write_text(json.dumps({'tag_name': 'v99.0', 'assets': [
                {'name': archive.name, 'size': archive.stat().st_size,
                 'browser_download_url': url + archive.name}]}))
            scenario = root / 'responses.json'
            scenario.write_text(json.dumps({
                'https://api.github.com/repos/Hydro74000/ngPost/releases/latest': str(release),
                url + 'manifest.json': str(root / 'server/manifest.json'),
                url + archive.name: str(archive)}))
            env = dict(os.environ, NGPOST_UPDATE_SCENARIO=str(scenario), QT_QPA_PLATFORM='offscreen')
            with (root / 'client.log').open('wb') as log:
                result = subprocess.run([str(client), '--update-handoff'], env=env,
                                        stdout=log, stderr=log, timeout=45)
            self.assertEqual(result.returncode, 0, (root / 'client.log').read_text())
            transactions = list(root.glob('.ngpost-update-*'))
            self.assertEqual(len(transactions), 1)
            work = transactions[0]
            for _ in range(600):
                if (work / 'error.txt').exists() or ((work / 'complete').exists()
                                                   and (install / 'restarted').exists()):
                    break
                time.sleep(0.05)
            self.assertFalse((work / 'cancelled').exists(), 'teardown must preserve successful handoff')
            error = (work / 'error.txt').read_text() if (work / 'error.txt').exists() else 'installer timed out'
            self.assertTrue((work / 'complete').exists(), error)
            self.assertTrue((install / 'restarted').exists())
            self.assertEqual((install / 'generation').read_text(), 'new')
            self.assertEqual(executable(install).read_bytes(), Path(os.environ['NGPOST_UPDATE_FIXTURE']).read_bytes())


if __name__ == '__main__':
    unittest.main()
