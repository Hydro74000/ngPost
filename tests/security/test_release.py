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


DELTA = Path(__file__).resolve().parents[2] / '.github/scripts/release-notes-delta.py'

# release_notes.txt as the release workflow reads it: the version in
# preparation first, bilingual, then the older versions.
NOTES = '''####################################################
###       Release: ngPost v9.1                   ###
###       date:    2026/09/24                    ###
####################################################

Intro paragraph.

====================================================
1. FUNCTIONAL CHANGES
====================================================

--- Improvements ---

- Parent feature:
  * old detail
    continued{sub}
- Other feature:
  * other detail{item}

====================================================
Notes de version (Français) :

--- Améliorations ---

- Fonction parente :
  * ancien détail{fr}

####################################################
###       Release: ngPost v9.0                   ###
###       date:    2026/09/01                    ###
####################################################

- Older release entry
'''
ADDITIONS = {'sub': '\n  * new detail\n    on two lines',
             'item': '\n- New feature:\n  * its detail',
             'fr': '\n  * nouveau détail'}
UNCHANGED = {'sub': '', 'item': '', 'fr': ''}


class ReleaseNotesDeltaTests(unittest.TestCase):
    def delta(self, before, after, previous='v9.1-unstable.1'):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            notes = root / 'release_notes.txt'

            def git(*args):
                subprocess.run(['git', '-c', 'user.name=t', '-c', 'user.email=t@t', *args],
                               cwd=root, check=True, capture_output=True)
            git('init', '-q')
            notes.write_text(before, encoding='utf-8')
            git('add', 'release_notes.txt')
            git('commit', '-q', '-m', 'previous')
            git('tag', 'v9.1-unstable.1')
            notes.write_text(after, encoding='utf-8')
            result = subprocess.run(['python3', str(DELTA), '--previous', previous], cwd=root,
                                    capture_output=True, text=True, encoding='utf-8', check=True)
            return result.stdout

    def test_only_the_increment_under_its_titles_and_parent_item(self):
        out = self.delta(NOTES.format(**UNCHANGED), NOTES.format(**ADDITIONS))
        for expected in ('1. FUNCTIONAL CHANGES', '--- Improvements ---', '- Parent feature:',
                         '  * new detail\n    on two lines', '- New feature:\n  * its detail',
                         'Notes de version (Français) :', '--- Améliorations ---',
                         '- Fonction parente :\n  * nouveau détail'):
            self.assertIn(expected, out)
        for absent in ('old detail', 'other detail', '- Other feature:', 'ancien détail',
                       'Intro paragraph', 'Older release entry', 'Release:', 'date:'):
            self.assertNotIn(absent, out)

    def test_a_reworded_entry_is_shown_whole(self):
        before = NOTES.format(**UNCHANGED)
        out = self.delta(before, before.replace('    continued', '    reworded'))
        self.assertIn('- Parent feature:\n  * old detail\n    reworded', out)
        self.assertNotIn('other detail', out)

    def test_no_new_entry_prints_nothing(self):
        notes = NOTES.format(**ADDITIONS)
        self.assertEqual(self.delta(notes, notes), '')

    def test_unreadable_previous_notes_give_the_whole_section(self):
        notes = NOTES.format(**ADDITIONS)
        out = self.delta(notes, notes, previous='v0-missing')
        for expected in ('Intro paragraph.', 'old detail', 'new detail', 'ancien détail'):
            self.assertIn(expected, out)
        self.assertNotIn('Older release entry', out)

    def test_first_build_of_a_new_version_gives_its_whole_section(self):
        older = NOTES.format(**UNCHANGED).split('####################################################\n'
                                               '###       Release: ngPost v9.0')[1]
        before = '####################################################\n###       Release: ngPost v9.0' + older
        out = self.delta(before, NOTES.format(**ADDITIONS))
        for expected in ('old detail', 'new detail', 'other detail', 'ancien détail'):
            self.assertIn(expected, out)
        self.assertNotIn('Older release entry', out)
