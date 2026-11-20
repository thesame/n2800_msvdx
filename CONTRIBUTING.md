# Contributing

Read the [architecture](docs/ARCHITECTURE.md) and [patch rationale](docs/PATCHES.md)
before changing the decoder. Graphics belongs to powervr_sgx; libdrm discovery
changes belong to libdrm. Keep those changes separately reviewable.

## Build and check

Use Debian 13 userspace dependencies:

```sh
sudo apt install build-essential pkg-config libva-dev libdrm-dev python3
make -j8 check
python3 tools/check-repository.py
```

Build kernel modules on a fast host against the target's exact headers,
configuration and `Module.symvers`. A matching release string is insufficient.
See [installation](docs/INSTALL.md) for module builds and a userspace sysroot.
All build products belong under ignored `build/`.

## Review requirements

Explain the concrete failure, resulting behavior and relevant limits in each
commit. Preserve input validation, no-fallback behavior, DMA ownership and bounded
failure handling. Changes to geometry require coordinated kernel, broker and
VA frontend updates. Kernel code is GPL-2.0-only; userspace and tools retain
MIT or their existing per-file notices. Preserve firmware and media notices.

Run [relevant tests](docs/TESTING.md), including exact pixels for decoder changes.
Retain reusable tests in the tree, but keep generated media, raw logs, sampled
playback output and machine-specific receipts outside Git. Report the source,
build identity, test method and outcome in the commit or review description.
Do not present successful decoding as proof of smooth or correct presentation.

Commit each successful step. For additions or removals, stage intended files
before updating the source manifest:

```sh
git add PATHS_YOU_INTEND_TO_COMMIT
python3 tools/check-repository.py --update-manifest
git add MANIFEST.sha256
python3 tools/check-repository.py
git diff --cached --check
```

The manifest updater includes tracked/staged files, not untracked build output.
CPU CI runs without contacting hardware. Installation, reboot and live suspend
are explicit operator actions; preserve a recoverable boot entry.
