#!/usr/bin/env python3
"""Turn a compile_commands.json into the one cppcheck should read.

cppcheck does not emulate a compiler: handed Qt's own headers it stops on their
#error checks (__WORDSIZE, byte order) and gives up whole files, which is how an
earlier run reported "0 defects" while analysing nearly nothing. It is meant to
learn a library from its description (--library=qt) instead. So this keeps our
sources, our include paths and the build directory (uic headers), and drops
generated sources and every Qt or system include directory.
"""
import argparse
import json
import os
import shlex
from pathlib import Path


def is_ours(path, root):
    return path == root or path.startswith(root + '/')


def main():
    parser = argparse.ArgumentParser(description='Prepare a compile database for cppcheck.')
    parser.add_argument('source', type=Path, help='compile_commands.json written by bear')
    parser.add_argument('output', type=Path)
    parser.add_argument('--root', type=Path, required=True, help='the src/ directory')
    args = parser.parse_args()
    root = str(args.root.resolve())

    kept = []
    for entry in json.loads(args.source.read_text()):
        name = Path(entry['file']).name
        if not is_ours(entry['file'], root) or name.startswith(('moc_', 'qrc_')):
            continue
        build = entry['directory']

        def wanted(path):
            path = os.path.normpath(os.path.join(build, path))
            return is_ours(path, root) or is_ours(path, build)

        argv = entry.get('arguments') or shlex.split(entry['command'])
        filtered = []
        index = 0
        while index < len(argv):
            arg = argv[index]
            if arg in ('-I', '-isystem') and index + 1 < len(argv):
                if wanted(argv[index + 1]):
                    filtered += ['-I', argv[index + 1]]
                index += 2
                continue
            if arg.startswith('-I') and not wanted(arg[2:]):
                index += 1
                continue
            filtered.append(arg)
            index += 1
        kept.append({'directory': entry['directory'], 'file': entry['file'],
                     'arguments': filtered})

    args.output.write_text(json.dumps(kept, indent=1))
    print(f'{len(kept)} translation units kept for cppcheck')


if __name__ == '__main__':
    main()
