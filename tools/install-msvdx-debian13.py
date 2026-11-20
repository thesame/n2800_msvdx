#!/usr/bin/env python3
"""Install the retained decoder on the pinned Debian SGX kernel; no reboot.

Run on the laptop with --stage /root/sgx-install after staging the matching
module, firmware and native userspace build. Keeps the stock boot entries.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stage', type=Path, required=True)
    ap.add_argument('--uid', type=int, default=1000)
    args = ap.parse_args()
    if os.geteuid() or os.uname().release != '7.3.3-sgx':
        ap.error('requires root on the pinned 7.3.3-sgx kernel')
    stage = args.stage.resolve()
    if not (stage / 'vendor-license.txt').is_file():
        ap.error('missing staged firmware vendor-license.txt')
    module = Path('/usr/lib/modules/7.3.3-sgx/kernel/drivers/gpu/drm/gma500/gma500_gfx.ko.xz')
    backup = stage / 'backup/gma500_gfx-before-msvdx.ko.xz'
    inputs = [stage / 'gma500_gfx.ko.xz',
              stage / 'msvdx_fw_mfld_DE2.0.bin',
              stage / 'msvdx-source/build/cedar_drv_video.so',
              stage / 'msvdx-source/build/cedar-decoder-broker']
    if not module.is_file() or not all(p.is_file() for p in inputs):
        ap.error('missing module or staged build inputs')
    # Use a dedicated entry: decoder options must never reach stock gma500.
    grub = Path('/boot/grub/grub.cfg').read_text()
    match = re.search(r"^menuentry 'Debian GNU/Linux'.*?^\}", grub, re.M | re.S)
    if not match or '/vmlinuz-7.3.3-sgx' not in match[0]:
        ap.error('default Debian entry is not the expected SGX kernel')
    entry = match[0].splitlines()
    entry[0] = "menuentry 'Debian SGX + Cedar MSVDX' --id cedar-msvdx {"
    options = ' gma500_gfx.msvdx=1'
    for i, line in enumerate(entry):
        if re.match(r'\s*linux\s', line):
            entry[i] += options
    backup.parent.mkdir(parents=True, exist_ok=True)
    if not backup.exists():
        shutil.copy2(module, backup)
    shutil.copyfile(inputs[0], module)
    fw = Path('/lib/firmware/cedarview-test')
    fw.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(inputs[1], fw / inputs[1].name)
    shutil.copyfile(stage / 'vendor-license.txt', fw / 'vendor-license.txt')
    dest = Path('/opt/cedar-msvdx')
    dest.mkdir(parents=True, exist_ok=True)
    for src in inputs[2:]:
        shutil.copy2(src, dest / src.name)
    Path('/etc/grub.d/41_cedar_msvdx').write_text(
        '#!/bin/sh\nexec tail -n +3 "$0"\n' + '\n'.join(entry) + '\n')
    Path('/etc/grub.d/41_cedar_msvdx').chmod(0o755)
    Path('/etc/default/grub.d').mkdir(exist_ok=True)
    Path('/etc/default/grub.d/90-cedar-msvdx.cfg').write_text('GRUB_DEFAULT=cedar-msvdx\n')
    Path('/etc/systemd/system/cedar-decoder.service').write_text(f'''[Unit]
Description=Cedarview MSVDX decoder broker (uid {args.uid})
ConditionPathExists=/dev/cedar-msvdx-test
After=systemd-modules-load.service

[Service]
RuntimeDirectory=cedar-decoder
RuntimeDirectoryMode=0711
ExecStart=/opt/cedar-msvdx/cedar-decoder-broker --socket /run/cedar-decoder/decoder.sock --uid {args.uid}

[Install]
WantedBy=multi-user.target
''')
    wrapper = Path('/usr/local/bin/cedar-vaapi')
    wrapper.write_text('''#!/bin/sh
export LIBVA_DRIVER_NAME=cedar
export LIBVA_DRIVERS_PATH=/opt/cedar-msvdx
export CEDAR_DECODER_SOCKET=/run/cedar-decoder/decoder.sock
export CEDAR_VA_DMABUF=1
exec "$@"
''')
    wrapper.chmod(0o755)
    for cmd in (['depmod', '7.3.3-sgx'], ['update-initramfs', '-u', '-k', '7.3.3-sgx'],
                ['update-grub'], ['systemctl', 'daemon-reload'],
                ['systemctl', 'enable', 'cedar-decoder.service']):
        subprocess.run(cmd, check=True)
    print('Installed; reboot required. Stock Debian and SGX boot entries retained.')


if __name__ == '__main__':
    main()
