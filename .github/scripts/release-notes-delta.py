#!/usr/bin/env python3
"""Print the release-notes entries a build adds since a previous release.

An unstable release describes its own increment, not the whole section of the
stable version in preparation: the entries of that section of
release_notes.txt added or changed since --previous, each under its section
titles and parent item so that it still reads on its own. Stable releases keep
the whole section (the release workflow's awk). Prints nothing when no entry
changed; prints the whole section when the previous notes cannot be read.
"""
import argparse
import difflib
from pathlib import Path
import re
import subprocess
import sys

# The same section the release workflow's awk extracts.
RELEASE = re.compile(r'^### +.*release:', re.IGNORECASE)
DATE = re.compile(r'^### +date:')
HASHES = re.compile(r'^#+$')
BANNER = re.compile(r'^={5,}\s*$')
SUBSECTION = re.compile(r'^-{3}\s+.+?\s+-{3}\s*$')
ITEM = re.compile(r'^[-*] ')
SUBITEM = re.compile(r'^\s+[-*] ')


def version_section(text):
    lines, inside = [], False
    for line in text.splitlines():
        if RELEASE.match(line):
            if inside:
                break
            inside = True
        elif inside and not DATE.match(line) and not HASHES.match(line):
            lines.append(line.rstrip())
    return lines


def blocks(lines):
    """Split a section into (kind, first, last) spans with their context.

    kind is 'title' (a banner title), 'subsection', 'item', 'subitem' or
    'text'. An item runs over its indented continuation lines.
    """
    spans, i = [], 0
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
        elif BANNER.match(line):
            title = i + 1 < len(lines) and lines[i + 1].strip() and not BANNER.match(lines[i + 1])
            if title:
                spans.append(('title', i + 1, i + 1))
                i += 3 if i + 2 < len(lines) and BANNER.match(lines[i + 2]) else 2
            else:
                i += 1
        elif SUBSECTION.match(line):
            spans.append(('subsection', i, i))
            i += 1
        else:
            kind = 'subitem' if SUBITEM.match(line) else 'item' if ITEM.match(line) else 'text'
            last = i
            while (last + 1 < len(lines) and lines[last + 1].strip()
                   and not ITEM.match(lines[last + 1]) and not SUBITEM.match(lines[last + 1])
                   and not BANNER.match(lines[last + 1]) and not SUBSECTION.match(lines[last + 1])
                   and (kind == 'text' or lines[last + 1].startswith(' '))):
                last += 1
            spans.append((kind, i, last))
            i = last + 1
    return spans


def changed_lines(old, new):
    matcher = difflib.SequenceMatcher(None, old, new, autojunk=False)
    return {j for tag, _, _, j1, j2 in matcher.get_opcodes() if tag in ('insert', 'replace')
            for j in range(j1, j2)}


def delta(old, new):
    changed = changed_lines(old, new)
    out, title, subsection, parent = [], None, None, None
    shown = set()

    def emit(span):
        if span in shown:
            return
        shown.add(span)
        kind, first, last = span
        if kind == 'title':
            out.extend(['', '=' * 52, new[first].strip(), '=' * 52, ''])
        elif kind == 'subsection':
            out.extend(['', new[first].strip(), ''])
        elif kind == 'text':
            out.extend(['', *new[first:last + 1], ''])
        else:
            out.extend(new[first:last + 1])

    for span in blocks(new):
        kind, first, last = span
        if kind == 'title':
            title, subsection, parent = span, None, None
            continue
        if kind == 'subsection':
            subsection, parent = span, None
            continue
        if kind == 'item':
            parent = span
        if not changed.intersection(range(first, last + 1)):
            continue
        for context in (title, subsection):
            if context:
                emit(context)
        if kind == 'subitem' and parent:
            emit(parent)
        emit(span)
    return re.sub(r'\n{3,}', '\n\n', '\n'.join(out)).strip('\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--previous', required=True, help='tag of the previous release')
    parser.add_argument('--notes', type=Path, default=Path('release_notes.txt'))
    args = parser.parse_args()
    new = version_section(args.notes.read_text(encoding='utf-8'))
    shown = subprocess.run(['git', 'show', f'{args.previous}:release_notes.txt'],
                           capture_output=True, text=True, encoding='utf-8', check=False)
    if shown.returncode != 0:
        print('\n'.join(new).strip('\n'))
        return 0
    text = delta(version_section(shown.stdout), new)
    if text:
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
