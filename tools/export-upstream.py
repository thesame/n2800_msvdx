#!/usr/bin/env python3
"""Export self-contained powervr_sgx and libdrm patches without changing their sources.

The supplied graphics checkout must contain the pinned dev commit. Output must
be a new directory under build/. No network, installation, submission or push.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import textwrap

ROOT = Path(__file__).resolve().parents[1]


def git(directory, *arguments):
    return subprocess.check_output(['git', '-C', str(directory), *arguments], text=True)


def commit(directory, subject, body):
    subprocess.run(['git', '-C', str(directory), 'add', '.'], check=True)
    subprocess.run(['git', '-C', str(directory), 'commit', '--signoff',
                    '-m', subject, '-m', '\n\n'.join(textwrap.fill(p, 72) for p in body.split('\n\n'))], check=True, stdout=subprocess.DEVNULL)


def identity(directory):
    for key in ('user.name', 'user.email'):
        value = git(ROOT, 'config', key).strip()
        if not value:
            raise SystemExit('Configure Git ' + key + ' before exporting')
        subprocess.run(['git', '-C', str(directory), 'config', key, value], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--graphics', type=Path, required=True)
    ap.add_argument('--output', type=Path, default=ROOT / 'build/upstream')
    args = ap.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to(ROOT / 'build') or output == ROOT / 'build':
        ap.error('output must be a new directory below build/')
    if output.exists():
        ap.error('output already exists; choose a new directory')
    selection = json.loads((ROOT / 'graphics-dev.lock.json').read_text())
    base = selection['commit']
    if git(args.graphics.resolve(), 'rev-parse', base + '^{commit}').strip() != base:
        ap.error('graphics checkout lacks the selected commit')
    # Reject mismatched graphics prerequisites rather than exporting a stale overlay.
    for patch in sorted((ROOT / 'kernel/gma500-patches').glob('*.patch')):
        upstream = git(args.graphics.resolve(), 'show', base + ':drm/gma500/' + patch.name)
        if upstream.encode() != patch.read_bytes():
            ap.error('graphics prerequisite differs: ' + patch.name)
    output.mkdir(parents=True)
    linux = output / 'linux-source'
    shutil.copytree(ROOT / 'kernel/base/linux-7.3.3', linux)
    subprocess.run(['git', 'init', '-q', str(linux)], check=True)
    identity(linux)
    commit(linux, 'gma500 build baseline',
           'Linux 7.3.3 gma500 source with the pinned powervr_sgx graphics prerequisites.')
    driver = linux / 'drivers/gpu/drm/gma500'
    subprocess.run(['patch', '--fuzz=0', '-p1', '-i',
                    str(ROOT / 'kernel/powervr-sgx/0001-drm-gma500-add-msvdx-h264-decoding.patch')],
                   cwd=driver, check=True, stdout=subprocess.DEVNULL)
    for name in ('cdv_msvdx.c', 'cdv_msvdx.h', 'cdv_h264_template.h'):
        shutil.copy2(ROOT / 'kernel' / name, driver / name)
    for name in ('cedar-msvdx.h', 'cedar-msvdx-dma.h'):
        shutil.copy2(ROOT / 'include' / name, driver / name)
    (linux / 'Documentation/gpu').mkdir(parents=True)
    shutil.copy2(ROOT / 'kernel/msvdx.rst', linux / 'Documentation/gpu/gma500-msvdx.rst')
    commit(linux, 'drm/gma500: add opt-in Cedarview MSVDX H.264 decoding',
           'Host the separate MSVDX engine in its PCI/display owner. Provide bounded '
           'H.264 submission, owned MMU mappings, read-only independent frame exports '
           'and a suspend checkpoint. Admission, root-only access and quarantine '
           'bound the private interface used by the companion VA backend and broker.\n\n'
           'Keep decoding disabled until msvdx=1. Include the runtime command/VLC '
           'template and interface documentation; no generated test picture or '
           'firmware binary is included. This targets the downstream powervr_sgx '
           'integration, not a new mainline media ABI.')
    linux_patch = git(linux, 'format-patch', '--stdout', '--no-signature', '--full-index', '-1')
    (output / 'gma500-msvdx.patch').write_text(linux_patch)
    graphics = output / 'graphics-source'
    subprocess.run(['git', 'clone', '-q', '--shared', '--no-checkout',
                    str(args.graphics.resolve()), str(graphics)], check=True)
    subprocess.run(['git', '-C', str(graphics), 'checkout', '-q', '--detach', base], check=True)
    identity(graphics)
    nested = graphics / 'drm/gma500/0100-drm-gma500-add-msvdx-h264-decoding.patch'
    nested.write_text(linux_patch)
    attributes = graphics / '.gitattributes'
    with attributes.open('a') as stream:
        stream.write('\n# Patch context includes leading spaces before kernel tabs.\n'
                     'drm/gma500/*.patch -whitespace\n')
    (graphics / 'docs/MSVDX.rst').write_bytes((ROOT / 'kernel/msvdx.rst').read_bytes())
    readme = graphics / 'drm/gma500/README.md'
    with readme.open('a') as stream:
        stream.write('\n`0100` adds opt-in MSVDX H.264 decoding (`gma500_gfx.msvdx=1`). '
                     'See `docs/MSVDX.rst` for its companion userspace, firmware '
                     'requirements and private interface limits.\n')
    commit(graphics, 'drm/gma500: integrate the opt-in MSVDX H.264 decoder',
           'Add a self-contained kernel patch to the existing gma500 build series. '
           'Keep the register/MMU/surface implementation, runtime template and shared '
           'private headers in that patch so both kernel build paths consume the '
           'same source without another checkout or copied build fixture.\n\n'
           'The interface remains root-only, explicitly enabled, limited to '
           'Cedarview MSVDX 2.1.3 and the supported progressive H.264 subset. '
           'Document lifetime, recovery, command provenance and the separately '
           'licensed firmware. No firmware binary is added to this repository.')
    patch = git(graphics, 'format-patch', '--stdout', '--no-signature', '--full-index',
                '--base=' + base, '-1')
    (output / 'powervr-sgx-msvdx.patch').write_text(patch)
    libdrm = output / 'libdrm'
    libdrm.mkdir()
    for patch in (ROOT / 'patches/libdrm').glob('*.patch'):
        shutil.copy2(patch, libdrm / patch.name)
    print('Prepared:', output / 'powervr-sgx-msvdx.patch')
    print('Standalone Linux patch:', output / 'gma500-msvdx.patch')
    print('Libdrm series:', libdrm)


if __name__ == '__main__':
    main()
