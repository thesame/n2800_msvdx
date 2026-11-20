# Patch rationale and integration boundaries

## Graphics baseline

`kernel/base/linux-7.3.3/` contains the gma500 sources needed for an out-of-tree
module build, with powervr_sgx's five graphics patches already applied.
`kernel/gma500-patches/` retains those patches with their original attribution;
do not apply them twice. Dependency identities are in
[provenance.json](../provenance.json) and
[graphics-dev.lock.json](../graphics-dev.lock.json).

| Graphics patch | Purpose |
| --- | --- |
| SGX auxiliary device | Give the render driver a child device and register/IRQ access while gma500 owns the shared PCI/display function; prevent conflicting SGX MMU ownership. |
| GEM DMA-BUF export | Share display buffers with the adjacent render device through PRIME. |
| Graphics-island wait | Allow up to 10 ms for island power-on rather than prematurely timing out. |
| Pipe enable retry | Check PLL/pipe state and retry a pipe that failed to start. |
| Disable memory self-refresh | Avoid underruns under GPU load with the existing fixed watermarks, at a possible power cost. |

These are external graphics prerequisites. The decoder's private NV12 exporter
is separate from gma500's display GEM exporter.

## MSVDX kernel host

SGX graphics does not implement video decoding. The MSVDX host runs inside
gma500 because it owns the shared PCI function and power lifecycle. Its hook
patch adds decoder registration/removal and a suspend checkpoint before parent
power-down. The host owns a separate register window, firmware, MMU, RENDEC,
bitstream allocations, image/motion storage and synchronous bounded submission.

The command layout follows the public Intel/Imagination DE2 definitions in
`sources/psb-video-de2/`; [de2-sources.lock.json](../de2-sources.lock.json) pins
those sources. DE2 uses an eight-word control-allocation header, linked DMA
records, explicit register address/value pairs and synchronous VLC preload.
Command framing must match the vendor firmware; firmware boot alone does not
prove picture decoding. The vendor binary is unchanged.

Runtime decoding submits validated caller data. The command template supplies
register layout, patch indices, VLC tables and backing capacities. Geometry is
coordinated across kernel allocation/MMU spans, broker quotas, export metadata
and VA image download. The fixed backing is 1920×1088 with stride 1920 and UV
offset 2088960; smaller coded rectangles retain that physical layout.

Only the runtime H.264 path is compiled. A single `msvdx=1` module parameter
enables registration; coherent independent surfaces, persistent sessions,
staged bindings and lazy readback are fixed defaults. Synthetic pictures,
reference pixels, raw register/readback diagnostics, fault injection and
unused kernel frame-validation interfaces are excluded. The 18 KiB template
contains the command layout and licensed VLC tables; the driver supplies the
caller’s bitstream and patches picture state before submission.

Completed immutable exports retain image/motion storage independently of the
engine session. Reader references prevent output-slot reuse. Shutdown and
suspend must prove quiescence before unmapping DMA pages; failures quarantine
the engine and allocations. Suspend retains the software DPB and storage and
reloads the firmware for the next submission.

A cold first-command failure permits one retry only if the fresh intra command
was unconsumed and a verified local reset drains requests. A completed-command
flag prevents sequence wrap from re-enabling that exception. Later faults
quarantine; there is no general command replay.

Coherent image allocations and matching cached read-only mappings make CPU
NV12 download practical. Producer completion invalidates the full image once;
immutable readers need ordering, not a second full-image cache walk. Motion
storage remains WC. Normal successful picture logs use dynamic debug to avoid
per-frame journal overhead.

## VA frontend and broker

The VA backend validates the supported progressive H.264 subset and translates
picture/slice/reference state into bounded packets. It supports reference B
pictures, full prediction lists and weights over four physical reference slots.
An envelope admits up to 64 consistent, ordered slices per picture. A separate
1 MiB input allocation and chained DMA descriptors avoid a 16-bit DMA length
limit; oversized broker payloads use sealed memfds with checked size and type.

The root broker permits one configured UID and one decoder client. Packets
contain file-local handles and codec state rather than arbitrary registers,
addresses or commands. Capabilities and layout must match before decoding.
Open-GOP leading B pictures lacking fresh references remain unreadable and are
never submitted or used as references. Unsupported input fails explicitly.

The root-only miscellaneous device and private ioctl ABI require userspace
from this tree. This integration targets powervr_sgx dev; mainline Linux would
require a standard media/DRM API design and its own review.

## Libdrm auxiliary-device discovery

The SGX DRM device is an auxiliary child of the display PCI function. Libdrm
must find the underlying PCI metadata while keeping the display DRM device
separate from its auxiliary render child. Otherwise wlroots rejects the node
or groups independent devices together, and Mesa can select software rendering.

The libdrm patch preserves the public PCI bus type and real parent PCI IDs.
Nodes belonging to the same auxiliary child are grouped together. Distinct
children and the parent remain distinct. No public bus enum or ABI is added.
The change belongs in libdrm so applications share one discovery fix.

## Playback configuration

mpv and labwc use their normal Debian binaries. Their launchers select the
private libdrm and enable `SGX_DEBUG=serial`. This workaround waits for producer
rendering and avoids tile tearing/reordering while graphics sharing lacks the
required synchronization. Libdrm discovery does not fix that synchronization.
Dropping the flag needs a graphics dependency/reuse-fence fix and visual checks;
zero reported drops cannot establish correctness.

The Wayland GPU launcher enables a decoder queue bounded to eight frames,
32 MiB and 0.5 seconds. It absorbs short decode/readback stalls while retaining
strict hardware selection. NV12 still crosses CPU memory; DMA-BUF export alone
does not establish zero-copy presentation. Native NV12 import, compositor layout
support and end-to-end fences remain future work.
