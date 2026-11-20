# Cedar Video

H.264 hardware decoding on Intel Cedarview (Atom N2800, PCI `8086:0be2`),
using the MSVDX decoder, a VA-API backend and the
[powervr_sgx graphics stack](https://github.com/The1TrueJoe/powervr_sgx).

The working playback path is **mpv on native Wayland**, with SGX GLES2 rendering.
Decoded NV12 is downloaded to the CPU and uploaded for presentation. Occasional
presentation drops remain; this is not a zero-copy implementation.

## Supported input

- Progressive 8-bit H.264 4:2:0, restricted Baseline/Main/High profiles.
- Coded rectangles up to 1920×1088, including cropped 1920×1080 output.
- Up to four short-term references, reference B pictures and weighted prediction.
- Up to 64 slices per picture; approximately 1 MiB total compressed picture data.

Interlaced input, 10-bit video, long-term references, custom scaling matrices
and other codecs are unsupported. Unsupported input fails explicitly; the
launchers disable software fallback. Browser acceleration is not provided.

## Playback

Follow [installation](docs/INSTALL.md) first. From a TTY, as the desktop user:

```sh
cedar-labwc -s alacritty
```

Inside that session:

```sh
cedar-mpv-wayland-gpu --fullscreen /path/to/video.mp4
```

Playback starts muted; add `--mute=no` for audio. The compositor and GPU player
launchers enable `SGX_DEBUG=serial`, required to avoid tile tearing and frame
reordering. A bounded decoder queue absorbs short stalls. `cedar-mpv-wayland`
provides a CPU presentation fallback. X11 GPU playback is available through
`cedar-mpv`, but performs poorly at 1080p.

## Development

```sh
sudo apt install build-essential pkg-config libva-dev libdrm-dev python3
make -j8 check
python3 tools/check-repository.py
```

These commands build userspace and run CPU checks without accessing hardware.
The decoder uses a private root-only kernel interface and a UID-restricted
broker; it is not a mainline Linux decoder interface.

Read [contributing](CONTRIBUTING.md), [architecture](docs/ARCHITECTURE.md),
[patch rationale](docs/PATCHES.md), [testing](docs/TESTING.md),
[upstream preparation](docs/UPSTREAM.md), [recovery](docs/RECOVERY.md) and [licensing](docs/LICENSING.md).
Dependency identities are in [provenance.json](provenance.json) and
[graphics-dev.lock.json](graphics-dev.lock.json).

The repository includes the decoder source, kernel build baseline, firmware,
patches, launchers, reusable tests and an attributed sample clip. Local build
products and test captures belong in ignored `build/`, outside the published tree.
