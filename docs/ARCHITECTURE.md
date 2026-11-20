# Architecture and source map

For per-change problems, implementation choices, evidence and remaining limits,
see [PATCHES.md](PATCHES.md).

```text
mpv / FFmpeg
  -> libva -> cedar_drv_video.so
      -> Unix socket -> root cedar-decoder-broker
          -> /dev/cedar-msvdx-test (private ABI)
              -> gma500_gfx MSVDX host -> DE2 firmware -> decoder engine

decoded NV12 -> CPU download -> player textures -> SGX GLES2 -> display
                                   or wlshm CPU presentation under Wayland
```

The GPU and decoder are separate engines. gma500 owns the shared PCI device
and display. The existing SGX patches publish an auxiliary device for
powervr_sgx; our MSVDX host uses its own register range and MMU/resources inside
the gma500 module. It polls bounded completion rather than integrating decoder
interrupts. Rebuilding this module is a kernel-code change, not a whole-kernel
rebuild. The new SGX kernel driver is an external dependency.

| Component | Entry point / responsibilities |
| --- | --- |
| `kernel/cdv_msvdx.c` | Firmware boot, MMU/RENDEC setup, runtime picture admission, surfaces, DMA-BUF export, bounded completion and quarantine |
| `include/cedar-msvdx.h` | Private ioctl/caps/packet/surface ABI; read validators, not just enum values |
| `src/va-driver.c` | Restricted H.264 VA profiles, picture/slice/DPB conversion, surface ownership, CPU image download and optional export |
| `src/decoder-transport.c` | Direct-root or brokered transport; explicit failure, no silent fallback |
| `src/decoder-broker.c`, `broker-wire.c` | Root single-client service, uid credential check, bounded protocol and fd passing |
| `include/cedar-broker.h` | Broker wire ABI |
| `kernel/cdv_h264_template.h` | DE2 runtime command layout, patch indices, VLC tables and backing capacities |
| `kernel/base/` | Complete gma500 source subset needed to build against the exact kernel headers |
| `patches/libdrm/` | Real PCI metadata for auxiliary children, child-aware node grouping; no public ABI extension |

The decoder is enabled with `gma500_gfx.msvdx=1`. Firmware boots on the first
submission. Image allocations are coherent, completed storage is immutable,
readback is lazy, and independent surfaces support staged output bindings.
These are the runtime configuration; bring-up modes and test pictures are absent.

In the installed independent-surface/lazy-readback mode, five NV12 backing
surfaces retain fixed 1920×1088 backing storage and stride 1920. Coded rectangles
may be smaller, with the UV plane always at byte 2088960. Export metadata carries
the coded dimensions and explicit UV offset; CPU download packs only the logical
rectangle. Each image occupies 765 pages and each private colocated-motion allocation
occupies 259 pages. The command arena and four reference images use separate,
size-derived MMU page-table spans, checked against the next reserved region.
The generated DE2 template uses row-stride mode 3 for the 1920-byte stride.
The decoder uses coherent allocations for image
pages and matching read-only cached CPU mappings. Private motion storage remains
WC, and cache maintenance still runs around ownership changes. It never creates
a cached alias of a WC allocation. At 1080p this bounds audio/video skew much
better than WC readback, while presentation remains a separate qualification.
`CEDAR_VA_DMABUF=1` opts into the export-capable backend needed by the broker;
it does not mean mpv is presenting decoded frames without CPU copies.

VA-API is userspace; `/dev/cedar-msvdx-test` is our kernel transport choice, not
a standard VA kernel interface. The root-only node plus broker are prototype
boundaries. Replacing this with V4L2 or an appropriately integrated DRM decoder
interface is future architecture work, not an installer option.

The capability-gated v13/v14 packets carry explicit retained-reference masks.
Version 14 carries full 32-entry prediction lists and weights; aliases can share
one of four physical reference slots. Runtime commands expand the qualified
DE2 template lists, with the expanded word count passed to firmware. POCs are
translated into bounded relative hardware values.

Version 14 uses a separate owned 1 MiB bitstream allocation and bounded chained
DE2 DMA descriptors, with at most 65,535 bytes per descriptor. The broker sends
large packets in a sealed memfd rather than exceeding Unix socket packet limits.
It validates immutable size, backing type and seals before mapping. The input
pages follow engine lifetime, including quarantine retention on failed shutdown.
Older capability sets keep their previous packet format and slice-size limit.

After seeking into an open GOP, unavailable leading B pictures before the fresh
intra origin are marked unreadable and never submitted or used as references.
The recovered visible output must still pass the full exact-pixel verifier.

The installed lazy/export path also admits a version-10 picture envelope of
2..64 version-14 slices. The broker bounds the complete envelope to
`CEDAR_H264_PACKET_LIMIT` (1,051,332 bytes), validates each child and rejects
nested envelopes, inconsistent pictures and non-monotonic macroblock starts.
The kernel validates the whole picture before submitting its first slice, binds
one output for all children, and exports only after every slice completes.
First-command recovery can retry the whole fresh intra envelope only when its
first command was unconsumed. A permanent completed-command flag prevents the
16-bit sequence counter's wrap from satisfying the startup-only abort guard.
