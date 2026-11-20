# Component licensing and provenance

This is a mixed-license handoff, not an assertion that every included byte is MIT.

| Material | Terms |
| --- | --- |
| Project userspace, headers and tools | Root MIT license, except retained per-file notices |
| Kernel host, H.264 command template and gma500 baseline | GPL-2.0-only / retained kernel notices; text in LICENSES/GPL-2.0-only.txt |
| DE2 command definitions and generator sources | Original Intel/Imagination notices retained in each file; hashes/URLs in de2-sources.lock.json |
| Decoder firmware | **Non-free vendor binary license**, reproduced unmodified in firmware/vendor-license.txt; not relicensed under MIT/GPL |
| Demo excerpt | Big Buck Bunny, Blender Foundation, CC BY 3.0; see demo/ATTRIBUTION.md |
| Optional external graphics packages | Their upstream Linux/Mesa/firmware licenses; pinned upstream source repository and commits recorded separately |

The included firmware is the matched 32,788-byte `msvdx_fw_mfld_DE2.0.bin`,
version 0x496. Its original accompanying license permits unmodified binary
redistribution subject to the stated conditions, restricts hardware use and
prohibits reverse engineering. Preserve the original notice and do not treat
the image as modifiable open firmware. No proprietary firmware disassembly is
required to build or use this handoff.

`provenance.json` identifies the kernel baseline, graphics dependency, decoder
command definitions, libdrm archive and unchanged firmware. Per-file notices
retain original attribution; MANIFEST.sha256 covers the distributed bytes.
