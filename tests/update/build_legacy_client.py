"""Build the unmodified updater shipped in stable 5.5.1, with a fake transport."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

COMMIT = 'e5cc72a8451f1d44891c347ea1cf030ff5ddf538'
REPO = Path(__file__).resolve().parents[2]


def build(destination, qmake):
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(['git', 'fetch', '--depth=1', 'origin', COMMIT], cwd=REPO, check=True)
    for name in ('UpdateChecker.h', 'UpdateChecker.cpp'):
        data = subprocess.check_output(['git', 'show', f'{COMMIT}:src/utils/{name}'], cwd=REPO)
        (destination / name).write_bytes(data)
    for name in ('NgPost.h', 'client.cpp'):
        shutil.copy2(REPO / 'tests/update/legacy' / name, destination / name)
    (destination / 'legacy.pro').write_text('''QT += core gui network
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = legacy-client
SOURCES += client.cpp UpdateChecker.cpp
HEADERS += UpdateChecker.h NgPost.h
''')
    subprocess.run([qmake, 'legacy.pro'], cwd=destination, check=True)
    command = ['nmake', '/NOLOGO'] if os.name == 'nt' else ['make', '-j2']
    subprocess.run(command, cwd=destination, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--qmake', default='qmake')
    args = parser.parse_args()
    build(args.destination.resolve(), args.qmake)
