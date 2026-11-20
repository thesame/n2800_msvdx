.. SPDX-License-Identifier: GPL-2.0-only

Cedarview MSVDX H.264 decoding
=============================

This optional gma500 host supports the MSVDX 2.1.3 decoder on Intel Atom N2800
(PCI 8086:0be2). Enable it with ``gma500_gfx.msvdx=1``. It uses a separate
register window and owned MMU mappings; it does not reset the SGX or display.
Firmware is loaded on the first picture submission.

Userspace and trust boundary
---------------------------

The companion cedar-video project supplies a VA-API backend and root broker.
The private ``/dev/cedar-msvdx-test`` interface remains mode 0600 and requires
CAP_SYS_RAWIO. The broker authorizes one desktop UID and serves one decoder
client. Root is trusted. This is a downstream interface for powervr_sgx, not
a stable mainline DRM/media ABI or a general multi-user decoder service.

Packets contain bounded H.264 picture descriptions and file-local surface
handles, not arbitrary commands, registers or physical addresses. The host
validates geometry, references, slice boundaries and limits before submission.
Unsupported input fails; the supplied player disables software fallback.

Input and storage
-----------------

Supported input is progressive 8-bit H.264 4:2:0 through coded 1920x1088,
including restricted Baseline/Main/High, four short-term references, reference
B pictures and weighted prediction. Interlacing, long-term references, custom
scaling matrices and other codecs are unsupported. A picture has at most 64
slices and approximately 1 MiB of compressed data including packet overhead.

Image backing uses stride 1920 and UV offset 2088960 even for smaller pictures.
Five bounded image/motion slots retain the decoded reference state. Completed
read-only DMA-BUFs retain storage independently of the decoder file and prevent
slot reuse until their readers retire. CPU download packs the logical rectangle.
Coherent image allocations match their cached read-only mappings; motion and
command storage use write-combined mappings with explicit cache maintenance.

Completion is polled synchronously with bounded waits. Closing or suspending
must prove engine quiescence before unmapping DMA pages. An unproved shutdown
quarantines the engine and retains its memory, device and module references;
reboot is the recovery. A single startup retry is limited to a fresh intra
command that was unconsumed before a verified isolated reset. Established
reference chains are not replayed. Suspend retains images, motion and software
DPB state; the next submission reloads firmware and scratch state.

Firmware and command provenance
-------------------------------

The unchanged vendor image is requested as
``cedarview-test/msvdx_fw_mfld_DE2.0.bin``, version 0x496, 32788 bytes,
SHA256 ``6d073592a1f10bc7da269f11fd8f15b6ca3e69c07d9efde4cdaa10f4af6be27b``.
It is supplied separately under its vendor redistribution license. No firmware
binary is included in this patch.

The runtime template and VLC tables derive from the published Intel/Imagination
DE2 psb_video source at commit fa6f2e9012a890942a47eae9f6f1a0ddcd97e9a4:

https://android.googlesource.com/platform/hardware/intel/img/psb_video/+/fa6f2e9012a890942a47eae9f6f1a0ddcd97e9a4/

The licensed notices remain with the tables. The template contains no sample
bitstream or reference image; picture state and caller input are supplied at
runtime. Firmware RAM transfer follows the public GPL Cedarview DDK
imgv/psb_msvdxinit.c; register offsets follow imgv/psb_msvdx.h.

Checking changes
----------------

Build against the exact target kernel configuration and Module.symvers.
The companion tools compare complete visible NV12 frames with software, check
hardware completion counters, reject unsupported controls and verify storage
retirement. Cover profiles, crops, 1088 coded lines, reference B pictures,
64-slice inputs, seek/reload and 16-bit firmware sequence wrap. A live-reference
suspend check requires an explicit operator request.

Presentation is separate from decoding. Native Wayland GLES2 playback still
uses CPU download/upload and requires SGX_DEBUG=serial in compositor and player
to avoid tile tearing/reordering. Zero dropped-frame counters do not detect
that corruption. Keep a bootable fallback when replacing gma500.
