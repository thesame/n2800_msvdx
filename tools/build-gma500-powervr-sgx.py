#!/usr/bin/env python3
"""Build the tracked Linux 7.3.3 gma500 baseline plus the MSVDX host.

--headers must identify the exact SGX kernel package/config/Module.symvers.
The runtime template is a build dependency. The private ABI accepts validated
H.264 picture descriptions and caller bitstreams.
Builds only: no installation, module loading, or reboot.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--linux', type=Path, default=ROOT / 'kernel/base/linux-7.3.3')
    ap.add_argument('--headers', type=Path,
                    required=True)
    ap.add_argument('--output', type=Path, default=ROOT / 'build/kernel')
    ap.add_argument('--jobs', type=int, default=6)
    args = ap.parse_args()

    src = args.linux.resolve() / 'drivers/gpu/drm/gma500'
    if not (src / 'psb_drv.c').is_file():
        ap.error(f'{src} is not a Linux gma500 source tree')
    if 'publish the Cedarview SGX as an auxiliary' not in (src / 'psb_drv.c').read_text(errors='ignore') \
            and 'aux' not in (src / 'Makefile').read_text(errors='ignore'):
        print('warning: pvrsgx gma500 patches do not look applied', flush=True)
    headers = args.headers.resolve()
    if (headers / 'include/config/kernel.release').read_text().strip() != '7.3.3-sgx':
        ap.error('expected matching 7.3.3-sgx headers')

    if not args.output.resolve().is_relative_to((ROOT / 'build').resolve()):
        ap.error('output must remain inside this repository build directory')

    # Rebuild the module directory from pristine inputs every time, so a stale
    # object cannot hide a changed source file.
    shutil.rmtree(args.output, ignore_errors=True)
    shutil.copytree(src, args.output)
    subprocess.run(['patch', '--fuzz=0', '-p1', '-i',
                    str(ROOT / 'kernel/powervr-sgx/0001-drm-gma500-add-msvdx-h264-decoding.patch')],
                   cwd=args.output, check=True)
    for name in ('cdv_msvdx.c', 'cdv_msvdx.h', 'cdv_h264_template.h'):
        shutil.copy2(ROOT / 'kernel' / name, args.output / name)
    shutil.copy2(ROOT / 'include/cedar-msvdx.h', args.output / 'cedar-msvdx.h')
    shutil.copy2(ROOT / 'include/cedar-msvdx-dma.h', args.output / 'cedar-msvdx-dma.h')

    subprocess.run(['make', '-C', str(headers), 'M=' + str(args.output.resolve()),
                    'CONFIG_DRM_GMA500=m', '-j' + str(args.jobs), 'modules'], check=True)
    ko = args.output / 'gma500_gfx.ko'
    vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', str(ko)], text=True).strip()
    if vermagic != '7.3.3-sgx SMP preempt mod_unload modversions':
        ap.error(f'unexpected vermagic: {vermagic}')
    params = subprocess.check_output(['modinfo', '-p', str(ko)], text=True)
    if 'msvdx:' not in params:
        ap.error('built module has no MSVDX parameters')
    report = {
        'kernel_release': '7.3.3-sgx',
        'module_symvers_sha256': sha(headers / 'Module.symvers'),
        'compiler_comment': subprocess.check_output(['readelf', '-p', '.comment', str(ko)], text=True).strip(),
        'linux_source': str(args.linux.resolve()),
        'headers': str(headers),
        'module_sha256': sha(ko),
        'vermagic': vermagic,
        'inputs': {
            'cdv_msvdx.c': sha(ROOT / 'kernel/cdv_msvdx.c'),
            'cdv_msvdx.h': sha(ROOT / 'kernel/cdv_msvdx.h'),
            'cedar-msvdx.h': sha(ROOT / 'include/cedar-msvdx.h'),
            'cedar-msvdx-dma.h': sha(ROOT / 'include/cedar-msvdx-dma.h'),
            'runtime-template': sha(ROOT / 'kernel/cdv_h264_template.h'),
            'gma500-patch': sha(ROOT / 'kernel/powervr-sgx/0001-drm-gma500-add-msvdx-h264-decoding.patch'),
        },
        'hardware_decode_verified': False,
    }
    (args.output / 'build-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
