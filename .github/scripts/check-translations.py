#!/usr/bin/env python3
"""Fail when lupdate finds messages absent from a tracked translation catalog."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def messages(path):
    return {
        (context.findtext('name'), message.findtext('source'),
         message.findtext('comment', ''), message.get('numerus', ''), message.get('id', ''))
        for context in ET.parse(path).findall('context')
        for message in context.findall('message')
        if message.find('translation') is None
        or message.find('translation').get('type') not in ('vanished', 'obsolete')
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lupdate', default='lupdate')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    source = args.root.resolve() / 'src'
    catalogs = sorted((source / 'lang').glob('ngPost_*.ts'))
    if not catalogs:
        parser.error('no ngPost translation catalogs found')
    # lupdate may rewrite locations, ordering and obsolete messages. Compare
    # message identities, not XML formatting, and never modify the checkout.
    with tempfile.TemporaryDirectory(prefix='ngpost-lupdate-') as directory:
        copies = []
        for catalog in catalogs:
            copy = Path(directory) / catalog.name
            shutil.copy2(catalog, copy)
            copies.append(copy)
        subprocess.run([args.lupdate, str(source), '-no-obsolete', '-locations', 'none',
                        '-ts', *map(str, copies)], check=True)
        missing = 0
        for catalog, copy in zip(catalogs, copies):
            new = messages(copy) - messages(catalog)
            missing += len(new)
            for context, text, comment, numerus, identifier in sorted(new):
                print(f'{catalog.name}: missing {context}: {text!r}'
                      f' (comment={comment!r}, numerus={numerus!r}, id={identifier!r})')
        if missing:
            print(f'{missing} missing catalog entries. Run lupdate, translate them, and regenerate the QM files.')
            return 1
    print('Translation extraction is complete in every catalog.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
