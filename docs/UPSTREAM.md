# Preparing integration patches

The kernel destination is **powervr_sgx dev**. Libdrm changes target libdrm's
own upstream. Keep these contributions separate; neither requires modifying
mpv or labwc source.

## Export a complete kernel integration

From cedar-video, with a local graphics checkout containing the selected dev
commit in `graphics-dev.lock.json`:

```sh
python3 tools/export-upstream.py --graphics /path/to/powervr_sgx \
  --output build/upstream
```

This creates:

- `build/upstream/powervr-sgx-msvdx.patch`: a Git mail patch against the pinned
  graphics dev commit, adding its gma500 kernel patch and decoder documentation.
- `build/upstream/gma500-msvdx.patch`: the underlying self-contained Linux patch
  against Linux 7.3.3 with the five graphics prerequisites already applied.
- `build/upstream/libdrm/`: the separate discovery and portable-test series.

The kernel patch includes every new source/header and the small command/VLC
template. Both graphics kernel build scripts already apply `drm/gma500/*.patch`;
no manual file copying or original workspace is required. Decoder registration
is disabled until `gma500_gfx.msvdx=1`. The vendor firmware is supplied separately
under its license and is excluded from the graphics patch.

The exporter uses isolated local repositories under its new output directory.
It reads Git's configured author identity, signs off its commits, checks the
pinned graphics prerequisites, and never changes or pushes the input checkout.
Review the generated patches and commit messages before submission. Configure
your own Git identity before exporting someone else's contribution.

## Review scope

The implementation uses a private root-only ABI, polling, one active decoder
client and fixed bounded backing storage. It targets the existing downstream
stack. Mainline Linux integration would require a media/DRM API design rather
than presenting this private interface as a standard decoder API.

Read [kernel interface and lifetime documentation](../kernel/msvdx.rst) and
[testing](TESTING.md). Report exact pixels, rejection controls, resource lifetime,
seek/reload and native Wayland presentation separately. Include source/build
identities and actual test commands in the review description. No capture logs
or personal target setup are needed in the submitted source tree.

powervr_sgx asks contributors to discuss larger additions in an issue before
submitting a PR to dev, preserve licenses, sign off and run hardware checks.
The exported files are preparation for that review, not an upstream submission.
Libdrm's shared-code changes require upstream review; its patch directory has
[build and test instructions](../patches/libdrm/README.md).
