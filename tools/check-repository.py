#!/usr/bin/env python3
"""Check the tracked source manifest and active documentation; no hardware access.

Stage intended new/deleted files before --update-manifest. Untracked local runs
are never silently included. Extracted source archives can be checked without Git.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def tracked_files():
    if (ROOT / '.git').exists():
        return sorted(p.decode() for p in subprocess.check_output(
            ['git', 'ls-files', '-z'], cwd=ROOT).split(b'\0') if p)
    return sorted(line.split('  ', 1)[1] for line in
                  (ROOT / 'MANIFEST.sha256').read_text().splitlines()) + ['MANIFEST.sha256']


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--update-manifest', action='store_true')
    args = ap.parse_args()
    names = sorted(n for n in tracked_files() if n != 'MANIFEST.sha256')
    if args.update_manifest and not (ROOT / '.git').exists():
        ap.error('refresh requires a Git checkout with intended files staged')
    missing = [n for n in names if not (ROOT / n).is_file()]
    if missing:
        raise SystemExit('Missing tracked files; stage deletions first: ' + ', '.join(missing))
    expected = ''.join(hashlib.sha256((ROOT / n).read_bytes()).hexdigest() + '  ' + n + '\n'
                       for n in names)
    manifest = ROOT / 'MANIFEST.sha256'
    if args.update_manifest:
        manifest.write_text(expected)
    elif manifest.read_text() != expected:
        raise SystemExit('Manifest differs. Stage intended changes, then run '
                         'python3 tools/check-repository.py --update-manifest')
    docs = [n for n in names if n.endswith('.md')]
    checked = 0
    errors = []
    for name in docs:
        p = ROOT / name
        for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)', p.read_text()):
            target = target.strip().strip('<>')
            url = urlsplit(target)
            if url.scheme or not url.path:
                continue
            q = (p.parent / unquote(url.path)).resolve()
            checked += 1
            if not q.is_relative_to(ROOT) or not q.exists():
                errors.append(name + ': ' + target)
    if errors:
        raise SystemExit('Broken local documentation targets:\n' + '\n'.join(errors))
    print(f'Checked {len(names)} manifest entries and {checked} documentation links.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        sys.exit(str(error))
