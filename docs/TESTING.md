# Testing

Run CPU tests with `make -j8 check` and check the distributed tree with
`python3 tools/check-repository.py`. These do not access hardware.

## Decoder correctness

On an authorized target with the broker running, stop other decoder clients
and run as the broker's permitted user:

```sh
python3 tools/verify-decode.py demo/big-buck-bunny-720p-safe-h264.mp4
python3 tools/verify-decode.py /path/to/video.mp4 --seek 120
```

The verifier compares the full requested visible NV12 output with software,
requires hardware completions and checks resource retirement and quarantine.
Its default is 64 frames, maximum 128; ensure sufficient output disk space.
Use strict errors and preserve the complete requested frame count.

Generate boundary inputs on the faster host using `tools/make-1080p-corpus.py`,
`tools/make-resolution-corpus.py` and `tools/make-stress-corpus.py`. Cover
Baseline/Main/High, B references, four references, cropped output, 1088 coded
lines, 64 slices, a 720p regression and paired oversized/rejected controls.
Use `--expect-rejection` for deliberately unsupported inputs.

For long streams and the 16-bit firmware sequence rollover:

```sh
python3 tools/make-stress-corpus.py build/stress
python3 tools/test-h264-stream.py build/stress/wrap-slices64.mp4 \
  --frames 1030 --expected-slices 64 --output build/wrap-check
```

The stream checker uses bounded pipes and frame hashes, without retaining
large uncompressed videos. FFmpeg and FFprobe are required.

## Presentation and graphics discovery

Inside the native Wayland session started by `cedar-labwc`:

```sh
python3 tools/test-libdrm-auxiliary.py
python3 tools/test-player.py /path/to/video.mp4 --output /var/tmp/cedar-player-check \
  --session wayland --fullscreen --vo gpu --audio --rounds 0 --play-seconds 120
```

The libdrm check tests fd/dev_t/enumeration APIs, PCI metadata, separate display
and auxiliary render identities, legacy APIs and descriptor retirement on the
Cedarview topology. It expects the private library loaded by the launcher.

The player check requires `vaapi-copy` throughout and records presentation and
decoder drops, sampled A/V skew, errors and hardware progress. `--rounds` above
zero adds seek/reload checks; `--alternate FILE` adds mixed resolutions/profiles.
Use `--play-start 120` for a matched scene, `--terminal` for interactive terminal
status output, and `--gpu-log` for a short renderer-identity check.

Inspect the image too: tile tearing or reordering can occur with zero reported
drops. Verify PowerVR SGX545 selection rather than llvmpipe, retain serial in
both compositor and player, and keep audio/rendering settings equal for paired
performance comparisons. A functional pass alone does not establish smoothness.

## Suspend continuation

Only when the operator explicitly requests a live suspend:

```sh
sudo python3 tools/test-h264-stream.py build/1080/high-coded1088.mp4 \
  --uid 1000 --frames 75 --suspend-after 30 --suspend-seconds 20 \
  --output /var/tmp/cedar-suspend-check
```

This actually suspends the machine using `rtcwake`. The same reference chain
continues after resume; all normalized frame hashes must match software.
The checker requires an unchanged boot ID, hardware progress and retired resources.
It never suspends without `--suspend-after`. Select the broker's actual UID.

## Failures

Stop on quarantine and retain logs outside Git. Do not unload the module or
free possibly active DMA pages. See [recovery](RECOVERY.md). A useful review
report identifies source commit, loaded module identity, exact kernel symbol
table, firmware, userspace, input, commands and outcome. Separate decoder pixel
correctness, presentation performance and visible graphics corruption.
