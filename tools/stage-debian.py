#!/usr/bin/env python3
"""Prepare installer inputs without changing the system or rebooting."""
import hashlib
import json
import lzma
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    osinfo = Path('/etc/os-release').read_text()
    if 'ID=debian' not in osinfo or 'VERSION_ID="13"' not in osinfo:
        raise SystemExit('stage the native userspace build on Debian 13')
    module = ROOT / 'build/kernel/gma500_gfx.ko'
    driver = ROOT / 'build/cedar_drv_video.so'
    broker = ROOT / 'build/cedar-decoder-broker'
    for p in [module, driver, broker]:
        if not p.is_file():
            raise SystemExit('build first: missing ' + str(p))
    if '__vaDriverInit_1_22' not in subprocess.check_output(['nm', '-D', str(driver)], text=True):
        raise SystemExit('VA library is not a native Debian libva 1.22 build')
    stage = ROOT / 'build/stage'
    (stage / 'msvdx-source/build').mkdir(parents=True, exist_ok=True)
    firmware = ROOT / 'firmware/msvdx_fw_mfld_DE2.0.bin'
    expected = json.loads((ROOT / 'provenance.json').read_text())['firmware']['sha256']
    if hashlib.sha256(firmware.read_bytes()).hexdigest() != expected:
        raise SystemExit('firmware checksum mismatch')
    (stage / 'gma500_gfx.ko.xz').write_bytes(lzma.compress(module.read_bytes(),
                                                        format=lzma.FORMAT_XZ,
                                                        check=lzma.CHECK_CRC32, preset=9))
    shutil.copy2(firmware, stage / firmware.name)
    shutil.copy2(ROOT / 'firmware/vendor-license.txt', stage / 'vendor-license.txt')
    for p in [driver, broker]:
        shutil.copy2(p, stage / 'msvdx-source/build' / p.name)
    print('Staged only:', stage)
    print('Installation changes the module/boot configuration; read docs/INSTALL.md first.')


if __name__ == '__main__':
    main()
