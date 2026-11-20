# Recovery

## Decoder fails or is quarantined

Stop the player. Read
`/sys/module/gma500_gfx/parameters/msvdx_quarantined` and preserve `journalctl -k -b`
and `journalctl -u cedar-decoder.service -b` before recovery. Do not retry a loop
of failing inputs. `Y` means the driver retained potentially active DMA memory;
the supported recovery is an authorized reboot, not forced freeing. The driver
may retry one fresh intra picture only after verifying that its first command
was unconsumed and the locally reset engine has no outstanding DMA requests.
This does not recover faults in an established reference chain. Quarantine also
prevents a normal decoder suspend checkpoint.

## Display/boot regression

Choose an original Debian kernel in GRUB, or the ordinary SGX entry without
MSVDX options. The installer creates a separate `Debian SGX + Cedar MSVDX`
entry and keeps the ordinary entries. Do not unload gma500 while the desktop
owns it. Keep network access/recovery media and the known-good kernel available.

To restore the original SGX module, use the backup saved under the **chosen
installer stage path** at `backup/gma500_gfx-before-msvdx.ko.xz`, copy it back
to `/usr/lib/modules/7.3.3-sgx/kernel/drivers/gpu/drm/gma500/gma500_gfx.ko.xz`,
then run `depmod 7.3.3-sgx` and `update-initramfs -u -k 7.3.3-sgx`.
Do not substitute another kernel's module. Disable `cedar-decoder.service`,
remove `/etc/default/grub.d/90-cedar-msvdx.cfg` and
`/etc/grub.d/41_cedar_msvdx`, and run `update-grub` to remove the dedicated
default entry. Reboot only after checking the fallback remains present.

## Private libdrm / Wayland regression

Exit `cedar-labwc` and launch ordinary `labwc`. Avoid `cedar-with-libdrm` for
clients. Debian's library was not replaced. Existing processes retain their
library mapping until exit. The private library lives in `/opt/cedar-libdrm`;
removing its launcher/session entry does not require changing the kernel.

For GPU tile tearing, retain `SGX_DEBUG=serial` through
`cedar-mpv` on X11 or `cedar-mpv-wayland-gpu` on Wayland, or use the CPU presentation fallback
`cedar-mpv-wayland`. Do not present llvmpipe as hardware GPU rendering.

## Package upgrades

Upgrading/replacing the SGX kernel package can overwrite the custom gma500
module, even if the kernel release string stays `7.3.3-sgx`. Rebuild against
the exact new headers/config/Module.symvers and requalify. New Mesa/libdrm
versions also require rechecking the private discovery patch and synchronization
workaround. Keep matching backups and build identities.


When rolling back a layout change, restore matching kernel module, userspace
VA library and broker together. Record your own backup locations before changing
them; backup names from the original qualification machine are not prerequisites.
Preserve a copy of the dedicated GRUB entry as well. A boot-option-only rollback
requires regenerating GRUB; a module rollback also requires `depmod` and rebuilding
the matching initramfs. The playback configuration uses coherent readback
and fullscreen native Wayland presentation.
