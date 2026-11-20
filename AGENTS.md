# Contributor and agent instructions

Read README.md, CONTRIBUTING.md, docs/ARCHITECTURE.md and docs/PATCHES.md first.

Keep this tree suitable for integration with powervr_sgx dev and upstream libdrm.
Retain source, reproducible build inputs, reusable tests and current documentation.
Do not add machine-specific receipts, experiment journals, raw captures or
historical reports. Use ignored build/ for local output.

The decoder supports progressive 8-bit H.264 through a coded 1920x1088 ceiling.
Change allocations, stride/UV layout, MMU spans, broker accounting, export metadata
and CPU download validation together when extending geometry. Require exact
software-reference pixels and paired rejection controls. Never substitute software
decoding silently or modify vendor firmware bytes.

Build on the faster host with exact target kernel headers/config/Module.symvers
and target-compatible userspace dependencies. Run make check and relevant hardware
checks. Commit each successful step. Maintain links and MANIFEST.sha256 using
tools/check-repository.py. Preserve original attribution and license notices.

Hardware access requires the current operator's authorization. Keep a bootable
fallback when installing modules or changing boot configuration. Live suspend
requires an explicit operator request. Stop on quarantine; do not free potentially
active DMA allocations. Access granted for one machine does not authorize another.
