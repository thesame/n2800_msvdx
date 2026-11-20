// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cedarview MSVDX H.264 decoder, hosted by the existing PCI/display owner.
 * Register addresses are from Intel's GPL Cedarview DDK imgv/psb_msvdx.h.
 * Firmware RAM transfer follows Intel/Imagination GPL Cedarview
 * imgv/psb_msvdxinit.c (Copyright 2011 Intel Corporation and Imagination).
 */
#include <drm/drm_cache.h>
#include <drm/drm_drv.h>
#include <linux/capability.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include "cdv_msvdx.h"
#include "cedar-msvdx-dma.h"
#include "cedar-msvdx.h"
#include "psb_drv.h"

MODULE_IMPORT_NS("DMA_BUF");
#include "cdv_h264_template.h"

#define CDV_MSVDX_OFFSET 0x90000
#define CDV_MSVDX_SIZE 0x10000
#define CDV_SGX_ID 0x001c
#define CDV_SGX_REV 0x0020
#define CDV_MSVDX_REV 0x0640
#define CDV_MSVDX_CLOCK 0x0620
#define CDV_MSVDX_RAM_BANK 0x06f0
#define MTX_ENABLE 0x0000
#define MTX_SOFT_RESET 0x0200
#define MTX_SYSC_CDMAC 0x0340
#define MTX_SYSC_CDMAA 0x0344
#define MTX_SYSC_CDMAS0 0x0348
#define MTX_SYSC_CDMAT 0x0350
#define MTX_RAM_DATA 0x0104
#define MTX_RAM_CONTROL 0x0108
#define MTX_RAM_STATUS 0x010c
#define MTX_DATA_BASE 0x82880000U
#define MTX_MEMORY_LIMIT (40U * 1024)
#define MSVDX_MMU_CONTROL 0x0680
#define MSVDX_MMU_BANK 0x0688
#define MSVDX_MMU_DIRECTORY 0x0694
#define MSVDX_MMU_REQUESTS 0x06d0
#define MSVDX_HOST_IRQ_ENABLE 0x0610
#define MSVDX_IRQ_STATUS 0x0608
#define MSVDX_COMMS_SIGNATURE 0x2fe0
#define CDV_COMMAND_VA 0x01000000U
#define CDV_H264_ARENA_VA 0x01010000U
#define CDV_H264_ARENA_SIZE                                                               \
	PAGE_ALIGN(CDV_H264_OUTPUT_OFFSET + CDV_H264_STRIDE * CDV_H264_HEIGHT * 3U / 2U + \
		   CDV_CODEC_AUX_BYTES)
#define CDV_SURFACE_SIZE PAGE_ALIGN(CDV_H264_STRIDE * CDV_H264_HEIGHT * 3U / 2U)
#define CDV_REFERENCES_VA 0x03000000U
#define CDV_BITSTREAM_VA 0x04000000U
#define CDV_REFERENCES_SIZE ((CDV_H264_SURFACES - 1U) * CDV_SURFACE_SIZE)
#define CDV_MMU_SPAN (4U * 1024 * 1024)
#define CDV_COMMAND_TABLE_BYTES \
	(DIV_ROUND_UP(CDV_H264_ARENA_SIZE + 16 * PAGE_SIZE, CDV_MMU_SPAN) * PAGE_SIZE)
#define CDV_REFERENCE_TABLE_BYTES (DIV_ROUND_UP(CDV_REFERENCES_SIZE, CDV_MMU_SPAN) * PAGE_SIZE)
#define CDV_RENDEC_VA 0x02000000U
#define CDV_RENDEC_A_SIZE (4U * 1024 * 1024)
#define CDV_RENDEC_B_SIZE (1U * 1024 * 1024)

static bool enable_msvdx;
module_param_named(msvdx, enable_msvdx, bool, 0400);
MODULE_PARM_DESC(msvdx, "Enable Cedarview H.264 decoding through the root-only private interface");

static unsigned int completed_slices, completed_frames;
static unsigned int large_slices, max_slice_bytes_seen;
static unsigned int firmware_boots, session_active;
static unsigned int firmware_ready_waits, firmware_ready_timeouts;
static unsigned int forced_resets, forced_reset_failures;
static unsigned int startup_retries, startup_retry_failures;
static unsigned int sleep_checkpoints, resume_restarts, resume_failures;
module_param_named(msvdx_sleep_checkpoints, sleep_checkpoints, uint, 0444);
module_param_named(msvdx_resume_restarts, resume_restarts, uint, 0444);
module_param_named(msvdx_resume_failures, resume_failures, uint, 0444);
module_param_named(msvdx_startup_retries, startup_retries, uint, 0444);
module_param_named(msvdx_startup_retry_failures, startup_retry_failures, uint, 0444);
module_param_named(msvdx_forced_resets, forced_resets, uint, 0444);
module_param_named(msvdx_forced_reset_failures, forced_reset_failures, uint, 0444);
module_param_named(msvdx_firmware_ready_waits, firmware_ready_waits, uint, 0444);
module_param_named(msvdx_firmware_ready_timeouts, firmware_ready_timeouts, uint, 0444);
module_param_named(msvdx_firmware_boots, firmware_boots, uint, 0444);
module_param_named(msvdx_session_active, session_active, uint, 0444);
module_param_named(msvdx_completed_slices, completed_slices, uint, 0444);
module_param_named(msvdx_completed_frames, completed_frames, uint, 0444);
MODULE_PARM_DESC(msvdx_completed_slices,
		 "Successful firmware completion replies, not pixel validation");
MODULE_PARM_DESC(msvdx_completed_frames,
		 "Pictures with all slice completions, not pixel validation");

static char *firmware_name = "cedarview-test/msvdx_fw_mfld_DE2.0.bin";
module_param_named(msvdx_firmware, firmware_name, charp, 0400);
MODULE_PARM_DESC(msvdx_firmware, "MSVDX firmware path");

static unsigned int readback_snapshots, unverified_loans;
module_param_named(msvdx_readback_snapshots, readback_snapshots, uint, 0444);
module_param_named(msvdx_unverified_loans, unverified_loans, uint, 0444);

static unsigned int surface_bindings;
module_param_named(msvdx_surface_bindings, surface_bindings, uint, 0444);
static atomic64_t next_storage_id = ATOMIC64_INIT(0);
static atomic_t surface_objects = ATOMIC_INIT(0), surface_pages = ATOMIC_INIT(0);
static atomic_t engine_contexts = ATOMIC_INIT(0);
static atomic_t bitstream_pages = ATOMIC_INIT(0);

static bool cdv_export_layout_valid(void)
{
	static_assert(CDV_H264_WIDTH == CEDAR_H264_MAX_WIDTH);
	static_assert(CDV_H264_HEIGHT == CEDAR_H264_MAX_HEIGHT);
	static_assert(CDV_H264_STRIDE == CEDAR_H264_STORAGE_STRIDE);
	static_assert(CDV_H264_SURFACES == CEDAR_H264_SURFACES);
	static_assert(CDV_H264_COLOCATED_BYTES == CEDAR_H264_COLOCATED_BYTES);
	return true;
}

struct cdv_msvdx_frame {
	struct page **pages;
	struct cdv_surface_storage *storage;
	struct drm_device *dev;
	u64 id;
	u32 npages, width, height, stride, uv_offset, verified_bytes, slot;
	bool cpu_cached;
};

struct cdv_dma_page {
	void *cpu;
	dma_addr_t dma;
	/* DMA allocation owns this page; exports retain the allocation's owner.
	 * Resolve the immutable allocation geometry once, not for every frame.
	 */
	struct page *export_page;
	bool coherent;
};

static void cdv_surface_page_alloc(struct drm_device *dev, struct cdv_dma_page *page, bool coherent)
{
	page->export_page = NULL;
	page->coherent = coherent;
	page->cpu = coherent ?
			    dma_alloc_coherent(dev->dev, PAGE_SIZE, &page->dma,
					       GFP_KERNEL | GFP_DMA32) :
			    dma_alloc_wc(dev->dev, PAGE_SIZE, &page->dma, GFP_KERNEL | GFP_DMA32);
}

static void cdv_surface_page_free(struct drm_device *dev, struct cdv_dma_page *page)
{
	if (page->coherent)
		dma_free_coherent(dev->dev, PAGE_SIZE, page->cpu, page->dma);
	else
		dma_free_wc(dev->dev, PAGE_SIZE, page->cpu, page->dma);
}

/* Storage owns DMA allocations. Engine page vectors below are non-owning aliases
 * in independent mode, preserving the already qualified device VA layout.
 */
struct cdv_surface_storage {
	struct kref refs;
	struct drm_device *dev;
	struct cdv_dma_page *pages;
	u32 allocated;
	atomic_t readers;
	u64 id;
};

static void cdv_storage_destroy(struct kref *ref)
{
	struct cdv_surface_storage *storage = container_of(ref, struct cdv_surface_storage, refs);
	u32 i;

	for (i = 0; i < storage->allocated; i++)
		cdv_surface_page_free(storage->dev, &storage->pages[i]);
	atomic_sub(storage->allocated, &surface_pages);
	atomic_dec(&surface_objects);
	drm_dev_put(storage->dev);
	kfree(storage->pages);
	kfree(storage);
}

static struct cdv_surface_storage *cdv_storage_alloc(struct drm_device *dev)
{
	struct cdv_surface_storage *storage;
	u32 i;
	int ret = -ENOMEM;
	u32 npages = CDV_SURFACE_SIZE / PAGE_SIZE + (CDV_H264_COLOCATED_BYTES / PAGE_SIZE);

	storage = kzalloc_obj(*storage);
	if (!storage)
		return ERR_PTR(-ENOMEM);
	kref_init(&storage->refs);
	storage->dev = dev;
	drm_dev_get(dev);
	atomic_set(&storage->readers, 0);
	atomic_inc(&surface_objects);
	storage->id = atomic64_inc_return(&next_storage_id);
	storage->pages = kcalloc(npages, sizeof(*storage->pages), GFP_KERNEL);
	if (!storage->pages)
		goto fail;
	for (i = 0; i < npages; i++) {
		struct cdv_dma_page *page = &storage->pages[i];

		cdv_surface_page_alloc(dev, page, i < CDV_SURFACE_SIZE / PAGE_SIZE);
		if (!page->cpu)
			goto fail;
		storage->allocated++;
		atomic_inc(&surface_pages);
		if (upper_32_bits(page->dma)) {
			ret = -ERANGE;
			goto fail;
		}
		{
			memset(page->cpu, i < CDV_SURFACE_SIZE / PAGE_SIZE ? 0xa5 : 0, PAGE_SIZE);
			drm_clflush_virt_range(page->cpu, PAGE_SIZE);
		}
	}
	return storage;
fail:
	kref_put(&storage->refs, cdv_storage_destroy);
	return ERR_PTR(ret);
}

/* Private, deliberately limited test protocol. No user command words, GPU
 * addresses or physical addresses enter the engine. Dimensions and progressive
 * picture flags still match the compiled template. This is not a stable UAPI.
 */
#define CDV_BATCH_FRAMES 8U
struct cdv_picture {
	u32 type, f_code, output, forward, backward, display;
};

struct cdv_reference_state {
	u32 last, previous, last_poc, previous_poc;
	bool p_refs, stable_refs;
	u32 dpb_slots[5], dpb_pocs[5], dpb_count, dpb_limit;
};

static void cdv_reference_reset(struct cdv_reference_state *state)
{
	*state = (struct cdv_reference_state){ .last = U32_MAX, .previous = U32_MAX };
}

struct cdv_job {
	u8 *packet, *output;
	bool owns_packet;
	bool hardware_complete;
	struct cdv_surface_storage *bound_output;
	struct cdv_job **children;
	u32 slice_count, first_mb, slice_index;
	u32 version, frames, flags, available;
	struct cdv_picture pictures[CDV_BATCH_FRAMES];
	u32 h264_offsets[CDV_BATCH_FRAMES], h264_qps[CDV_BATCH_FRAMES];
	u32 h264_cabac_init[CDV_BATCH_FRAMES];
	u32 h264_pocs[CDV_BATCH_FRAMES][3], h264_direct[CDV_BATCH_FRAMES];
	struct cedar_h264_config config;
	struct cedar_h264_weights weights;
	struct cedar_h264_p_refs p_refs;
	struct cedar_h264_refs refs;
	struct cedar_h264_dpb dpb;
	struct cedar_h264_dpb32 full;
	struct cedar_h264_weights full_weights[32];
	u32 dpb_pocs[5];
	struct cedar_h264_weights weights1;
	struct cedar_h264_weights weights_extra[2];
	u32 width, height, frame_bytes;
	struct cdv_reference_state next_references;
	u32 lengths[CDV_BATCH_FRAMES * CDV_H264_SLICE_COUNT];
	u32 offsets[CDV_BATCH_FRAMES * CDV_H264_SLICE_COUNT];
};

struct cdv_engine_context;
struct cdv_session {
	struct cdv_job *job;
	struct cdv_engine_context *engine;
	struct drm_device *dev;
	void __iomem *regs;
	struct cdv_reference_state references;
	struct cedar_h264_config config;
	bool config_valid;
	loff_t read_position;
	struct cdv_surface_storage *handles[CEDAR_SURFACE_HANDLES];
	u32 pending_handle, pending_slot;
};

static DEFINE_MUTEX(cdv_msvdx_lock);
static struct drm_device *cdv_msvdx_dev;
static struct cdv_session *cdv_active_session;
/* Failed independent-engine shutdown deliberately retains all resources until
 * reboot, including BAR mapping, runtime power and module/device references.
 */
static struct cdv_engine_context *cdv_quarantine_engine;
static void __iomem *cdv_quarantine_regs;
static bool engine_power_retained;
static bool cdv_msvdx_registered, cdv_msvdx_quarantined;
static unsigned int shared_frames, shared_acquires, shared_rejected_writes;
static atomic64_t next_frame_id = ATOMIC64_INIT(0);
static atomic_t exported_frames = ATOMIC_INIT(0), frame_exports = ATOMIC_INIT(0),
		frame_mmaps = ATOMIC_INIT(0);
static int export_count_get(char *buffer, const struct kernel_param *parameter)
{
	return scnprintf(buffer, PAGE_SIZE, "%u\n", (u32)atomic_read(parameter->arg));
}

static const struct kernel_param_ops export_count_ops = { .get = export_count_get };
module_param_cb(msvdx_exported_frames, &export_count_ops, &exported_frames, 0444);
module_param_cb(msvdx_frame_exports, &export_count_ops, &frame_exports, 0444);
module_param_cb(msvdx_frame_mmaps, &export_count_ops, &frame_mmaps, 0444);
module_param_cb(msvdx_surface_objects, &export_count_ops, &surface_objects, 0444);
module_param_cb(msvdx_surface_pages, &export_count_ops, &surface_pages, 0444);
module_param_cb(msvdx_engine_contexts, &export_count_ops, &engine_contexts, 0444);
module_param_cb(msvdx_bitstream_pages, &export_count_ops, &bitstream_pages, 0444);
module_param_named(msvdx_large_slices, large_slices, uint, 0444);
module_param_named(msvdx_max_slice_bytes_seen, max_slice_bytes_seen, uint, 0444);
module_param_named(msvdx_engine_power_retained, engine_power_retained, bool, 0444);
module_param_named(msvdx_shared_frames, shared_frames, uint, 0444);
module_param_named(msvdx_shared_acquires, shared_acquires, uint, 0444);
module_param_named(msvdx_shared_rejected_writes, shared_rejected_writes, uint, 0444);
module_param_named(msvdx_quarantined, cdv_msvdx_quarantined, bool, 0444);

struct cdv_firmware_image {
	u32 version, text_words, data_words, data_offset;
	const u8 *text, *data;
};

/* Decoder-local reset sequence from psb_msvdx_reset. Never reset SGX/display. */
static int cdv_reset_decoder(void __iomem *regs)
{
	u32 value;
	int ret;

	/* The Cedarview reset path explicitly enables every decoder clock before
	 * pausing/draining the MMU, including after firmware panic.
	 */
	writel(0x7f, regs + CDV_MSVDX_CLOCK);
	writel(2, regs + MSVDX_MMU_CONTROL);
	ret = readl_poll_timeout(regs + MSVDX_MMU_REQUESTS, value, !(value & 0xff), 10, 100000);
	if (ret)
		return ret;
	writel(readl(regs + 0x086c) | BIT(27), regs + 0x086c);
	writel(~0x100U, regs + 0x0600);
	readl(regs + 0x0600);
	writel(0, regs + 0x0600);
	ret = readl_poll_timeout(regs + MSVDX_MMU_REQUESTS, value, !(value & 0xff), 10, 100000);
	if (ret)
		return ret;
	writel(0x100, regs + 0x0600);
	ret = readl_poll_timeout(regs + 0x0600, value, !(value & 0x100), 10, 100000);
	if (!ret) {
		writel(0, regs + MSVDX_HOST_IRQ_ENABLE);
		writel(U32_MAX, regs + 0x060c);
	}
	return ret;
}

static int cdv_parse_firmware(const struct firmware *fw, struct cdv_firmware_image *image)
{
	u32 data_location;
	u64 size;

	if (fw->size < 16)
		return -EINVAL;
	image->version = get_unaligned_le32(fw->data);
	image->text_words = get_unaligned_le32(fw->data + 4);
	image->data_words = get_unaligned_le32(fw->data + 8);
	data_location = get_unaligned_le32(fw->data + 12);
	if (image->version != 0x496 || !image->text_words || !image->data_words ||
	    image->text_words > MTX_MEMORY_LIMIT / 4 || image->data_words > MTX_MEMORY_LIMIT / 4 ||
	    data_location < MTX_DATA_BASE || (data_location & 3))
		return -EINVAL;
	image->data_offset = data_location - MTX_DATA_BASE;
	if ((u64)image->data_offset + image->data_words * 4ULL > MTX_MEMORY_LIMIT ||
	    image->text_words * 4ULL > image->data_offset)
		return -EINVAL;
	size = 16 + ((u64)image->text_words + image->data_words) * 4;
	if (size != fw->size)
		return -EINVAL;
	image->text = fw->data + 16;
	image->data = image->text + image->text_words * 4;
	return 0;
}

static int cdv_ram_transfer(void __iomem *regs, u32 bank_size, u32 memory, u32 address, u32 words,
			    const u8 *bytes, bool verify)
{
	u32 saved = readl(regs + MTX_RAM_CONTROL);
	u32 bank = U32_MAX, value;
	int ret = 0;
	u32 i;

	for (i = 0; i < words; ++i, address += 4) {
		u32 next_bank = memory + address / bank_size;

		ret = readl_poll_timeout(regs + MTX_RAM_STATUS, value, value == 1, 5, 5000);
		if (ret)
			break;
		if (next_bank != bank) {
			if (next_bank > 0xff || address > 0xffffc) {
				ret = -EINVAL;
				break;
			}
			writel((next_bank << 20) | address | 2 | (verify ? 1 : 0),
			       regs + MTX_RAM_CONTROL);
			bank = next_bank;
		}
		if (verify) {
			ret = readl_poll_timeout(regs + MTX_RAM_STATUS, value, value == 1, 5, 5000);
			if (ret)
				break;
			if (readl(regs + MTX_RAM_DATA) != get_unaligned_le32(bytes + i * 4)) {
				pr_info("cedar-msvdx: ram_mismatch memory=%x address=%x word=%u\n",
					memory, address, i);
				ret = -EILSEQ;
				break;
			}
		} else {
			writel(get_unaligned_le32(bytes + i * 4), regs + MTX_RAM_DATA);
			ret = readl_poll_timeout(regs + MTX_RAM_STATUS, value, value == 1, 5, 5000);
			if (ret)
				break;
		}
	}
	writel(saved, regs + MTX_RAM_CONTROL);
	return ret;
}

/* Submit a DE2 render packet. Validate firmware ring descriptors before MMIO. */
static int cdv_submit_command(struct drm_device *dev, void __iomem *regs, dma_addr_t directory_dma,
			      u16 sequence, u32 operating_mode, bool first_slice, u32 command_words)
{
	u32 message[5] = {
		20 | (0x81 << 8) | ((u32)sequence << 16),
		(command_words << 16) | CDV_CODEC_MESSAGE_FLAGS,
		CDV_COMMAND_VA + 0xc00,
		lower_32_bits(directory_dma) | 1,
		operating_mode,
	};
	u32 tx = readl(regs + 0x2ff0), rx = readl(regs + 0x2fe4);
	u32 tx_size = tx & 0xffff, tx_base = (tx >> 16) + 0x2000;
	u32 rx_size = rx & 0xffff, rx_base = (rx >> 16) + 0x2000;
	u32 rd, wr, value, header, bytes, words, i, message_words = 5, replies;
	int ret;

	if (CDV_CODEC_MODE == 1 && !first_slice)
		message[1] &= ~1U; /* Published FW_VA_RENDER_IS_FIRST_SLICE. */

	if (sequence == 1)
		dev_info(dev->dev, "cedar-msvdx: ring tx=%08x rx=%08x\n", tx, rx);
	if (tx_size < message_words + 1 || rx_size < 2 || (tx_base & 3) || (rx_base & 3) ||
	    tx_base >= 0x2fd0 || rx_base >= 0x2fd0 || tx_size > (0x2fd0 - tx_base) / 4 ||
	    rx_size > (0x2fd0 - rx_base) / 4)
		return -EPROTO;
	rd = readl(regs + 0x2ff4);
	wr = readl(regs + 0x2ffc);
	if (rd >= tx_size || wr >= tx_size || rd != wr)
		return -EBUSY;
	if (wr + message_words > tx_size) {
		writel((tx_size - wr) * 4, regs + tx_base + wr * 4);
		writel(0, regs + 0x2ffc);
		writel(1, regs + 0x0080);
		ret = readl_poll_timeout(regs + 0x2ff4, value, value == 0, 10, 100000);
		if (ret)
			goto report;
		wr = 0;
	}

	for (i = 0; i < message_words; ++i) {
		writel(message[i], regs + tx_base + wr * 4);
		wr = (wr + 1) % tx_size;
	}
	writel(wr, regs + 0x2ffc);
	writel(1, regs + 0x0080); /* MTX_KICK. */
	ret = -ETIMEDOUT;
	for (replies = 0; replies < 16; ++replies) {
		ret = readl_poll_timeout(regs + 0x2fec, value, value != readl(regs + 0x2fe8), 50,
					 500000);
		if (ret)
			break;
		rd = readl(regs + 0x2fe8);
		wr = readl(regs + 0x2fec);
		if (rd >= rx_size || wr >= rx_size) {
			ret = -EPROTO;
			break;
		}
		header = readl(regs + rx_base + rd * 4);
		bytes = header & 0xff;
		words = DIV_ROUND_UP(bytes, 4);
		if (bytes < 4 || words >= rx_size || words > (wr + rx_size - rd) % rx_size) {
			ret = -EPROTO;
			break;
		}
		for (i = 0; i < words && (sequence == 1 || ((header >> 8) & 0xff) != 0xc0); ++i)
			dev_info(dev->dev, "cedar-msvdx: reply%u[%u]=%08x\n", replies, i,
				 readl(regs + rx_base + ((rd + i) % rx_size) * 4));
		writel((rd + words) % rx_size, regs + 0x2fe8);
		if (((header >> 8) & 0xff) == 0xc4) { /* ACK is not completion. */
			ret = -EINPROGRESS;
			continue;
		}
		ret = ((header >> 8) & 0xff) == 0xc0 && (header >> 16) == sequence ? 0 : -EIO;
		break;
	}

report:
	if (!ret)
		completed_slices++;
	if (ret || sequence == 1)
		dev_info(dev->dev,
			 "cedar-msvdx: stage=command_completion result=%d sequence=%u irq=%08x error=%08x fw_status=%08x tx_rd=%u tx_wr=%u video_decode=0\n",
			 ret, sequence, readl(regs + MSVDX_IRQ_STATUS), readl(regs + 0x2884),
			 readl(regs + 0x2fd0), readl(regs + 0x2ff4), readl(regs + 0x2ffc));
	if (sequence == 1 || ret) {
		dev_info(dev->dev,
			 "cedar-msvdx: mmu_status=%08x mmu_control=%08x mmu_bank=%08x directory=%08x core=%08x\n",
			 readl(regs + 0x068c), readl(regs + MSVDX_MMU_CONTROL),
			 readl(regs + MSVDX_MMU_BANK), readl(regs + MSVDX_MMU_DIRECTORY),
			 readl(regs + 0x0600));
		dev_info(dev->dev, "cedar-msvdx: directories=%08x,%08x,%08x,%08x\n",
			 readl(regs + 0x0694), readl(regs + 0x0698), readl(regs + 0x069c),
			 readl(regs + 0x06a0));

		dev_info(dev->dev,
			 "cedar-msvdx: mpeg2_regs sr=%08x sr_bytes=%08x entdec=%08x vlc_index=%08x fe_pps0=%08x fe_pps1=%08x fe_slice=%08x rendec_status=%08x\n",
			 readl(regs + 0x0818), readl(regs + 0x081c), readl(regs + 0x080c),
			 readl(regs + 0x08ec), readl(regs + 0x0e0c), readl(regs + 0x0e10),
			 readl(regs + 0x0e14), readl(regs + 0x089c));
		dev_info(dev->dev, "cedar-msvdx: mtx_dma control=%08x address=%08x status=%08x\n",
			 readl(regs + MTX_SYSC_CDMAC), readl(regs + MTX_SYSC_CDMAA),
			 readl(regs + MTX_SYSC_CDMAS0));
		for (i = 0; i < 4; ++i)
			dev_info(dev->dev,
				 "cedar-msvdx: dmac%u setup=%08x count=%08x status=%08x list=%08x\n",
				 i, readl(regs + 0x0500 + i * 32), readl(regs + 0x0504 + i * 32),
				 readl(regs + 0x050c + i * 32), readl(regs + 0x0510 + i * 32));
	}
	return ret;
}

/* Upload the unchanged vendor firmware through driver-owned mappings. */
static int cdv_dma_transfer(struct drm_device *dev, void __iomem *regs, u32 source_va,
			    u32 destination, u32 words, const u8 *expected)
{
	u32 saved = readl(regs + MTX_RAM_CONTROL), value;
	u32 bank_size = 1U << (((readl(regs + CDV_MSVDX_RAM_BANK) >> 16) & 0xf) + 2);
	int ret;

	/* Original driver's host-to-MTX firmware-upload path. MTX stays stopped. */
	if (!words || words > MTX_MEMORY_LIMIT / 4 || destination > MTX_MEMORY_LIMIT - words * 4)
		return -EINVAL;
	writel(6, regs + 0x06f0);
	ret = readl_poll_timeout(regs + 0x06f0, value, !(value & 0x18), 5, 10000);
	if (ret)
		goto release;
	writel(destination, regs + MTX_SYSC_CDMAA);
	writel(0x04010000 | words, regs + MTX_SYSC_CDMAC); /* Burst=4, enable. */
	writel(readl(regs + 0x0600) & ~BIT(12), regs + 0x0600);
	writel(0, regs + 0x050c);
	writel(source_va, regs + 0x0500);
	writel(words, regs + 0x0504);
	writel(0x02000000, regs + 0x0508);
	writel(MTX_SYSC_CDMAT, regs + 0x0514);
	writel(BIT(16) | words, regs + 0x0504);
	ret = readl_poll_timeout(regs + 0x050c, value, value & BIT(17), 10, 100000);
	if (!ret)
		ret = readl_poll_timeout(regs + MTX_SYSC_CDMAS0, value, value & 1, 10, 10000);
release:
	writel(saved, regs + MTX_RAM_CONTROL);
	writel(4, regs + 0x06f0);
	if (!ret)
		ret = cdv_ram_transfer(regs, bank_size, 0x10, destination, words, expected, true);
	dev_info(dev->dev,
		 "cedar-msvdx: stage=dma_copy result=%d verified_bytes=%u dma_status=%08x dma_count=%08x irq=%08x mmu_status=%08x mmu_control1=%08x video_decode=0\n",
		 ret, ret ? 0 : words * 4, readl(regs + 0x050c), readl(regs + 0x0504),
		 readl(regs + MSVDX_IRQ_STATUS), readl(regs + 0x068c), readl(regs + 0x0684));
	return ret;
}

static void cdv_arena_write(struct cdv_dma_page *pages, u32 offset, const u8 *source, u32 bytes)
{
	while (bytes) {
		u32 chunk = min(bytes, (u32)PAGE_SIZE - offset % PAGE_SIZE);

		memcpy((u8 *)pages[offset / PAGE_SIZE].cpu + offset % PAGE_SIZE, source, chunk);
		offset += chunk;
		source += chunk;
		bytes -= chunk;
	}
}

static void cdv_flush_arena(struct cdv_dma_page *pages)
{
	u32 i;

	for (i = 0; i < CDV_H264_ARENA_SIZE / PAGE_SIZE; ++i)
		drm_clflush_virt_range(pages[i].cpu, PAGE_SIZE);
}

static void cdv_snapshot_at(struct cdv_dma_page *arena, u32 base, u8 *output, u32 width, u32 height)
{
	u32 row, offset, remaining, chunk;

	readback_snapshots++;
	dma_rmb();
	for (row = base / PAGE_SIZE; row < (base + CDV_SURFACE_SIZE) / PAGE_SIZE; ++row)
		drm_clflush_virt_range(arena[row].cpu, PAGE_SIZE);
	for (row = 0; row < height * 3 / 2; ++row) {
		offset = base +
			 (row < height ? row : (CDV_H264_HEIGHT) + row - height) * CDV_H264_STRIDE;
		remaining = width;
		while (remaining) {
			chunk = min(remaining, (u32)PAGE_SIZE - offset % PAGE_SIZE);
			memcpy(output, (u8 *)arena[offset / PAGE_SIZE].cpu + offset % PAGE_SIZE,
			       chunk);
			output += chunk;
			offset += chunk;
			remaining -= chunk;
		}
	}
}

/* Decoder-owned DMA resources, kept separate from per-submit CPU buffers. */
struct cdv_engine_context {
	dma_addr_t directory_dma, table_dma, command_dma, rendec_tables_dma;
	dma_addr_t reference_table_dma, firmware_dma;
	__le32 *directory, *table, *command, *rendec_tables, *reference_table;
	struct cdv_dma_page *rendec_pages, *arena, *reference_pages;
	struct cdv_surface_storage *surfaces[CEDAR_H264_SURFACES];
	u8 *firmware_buffer;
	u32 rendec_allocated, arena_allocated, references_allocated;
	dma_addr_t bitstream_table_dma;
	__le32 *bitstream_table;
	struct cdv_dma_page *bitstream;
	u32 bitstream_allocated;
	u32 saved_control, saved_bank, saved_directory[4], saved_irq, saved_clock;
	u16 next_sequence;
	bool mapped, command_completed, needs_restart;
};

static unsigned int cdv_engine_readers(struct cdv_engine_context *engine)
{
	u32 i, readers = 0;

	if (engine)
		for (i = 0; i < ARRAY_SIZE(engine->surfaces); i++)
			if (engine->surfaces[i])
				readers += atomic_read(&engine->surfaces[i]->readers);
	return readers;
}

static unsigned int cdv_slot_readers(struct cdv_engine_context *engine, u32 slot)
{
	return engine && engine->surfaces[slot] ? atomic_read(&engine->surfaces[slot]->readers) : 0;
}

static struct cdv_dma_page *cdv_arena_storage_page(struct cdv_engine_context *engine, u32 index)
{
	u32 offset = index * PAGE_SIZE, slot;

	if (engine->surfaces[0] && offset >= CDV_H264_OUTPUT_OFFSET &&
	    offset < CDV_H264_OUTPUT_OFFSET + CDV_SURFACE_SIZE)
		return &engine->surfaces[0]->pages[(offset - CDV_H264_OUTPUT_OFFSET) / PAGE_SIZE];
	if (CDV_H264_COLOCATED_BYTES && offset >= CDV_H264_COLOCATED_BASE - CDV_H264_ARENA_VA &&
	    offset < CDV_H264_COLOCATED_BASE - CDV_H264_ARENA_VA +
			     CDV_H264_SURFACES * CDV_H264_COLOCATED_BYTES) {
		offset -= CDV_H264_COLOCATED_BASE - CDV_H264_ARENA_VA;
		slot = offset / max_t(u32, CDV_H264_COLOCATED_BYTES, 1);
		if (engine->surfaces[slot])
			return &engine->surfaces[slot]
					->pages[CDV_SURFACE_SIZE / PAGE_SIZE +
						(offset % max_t(u32, CDV_H264_COLOCATED_BYTES, 1)) /
							PAGE_SIZE];
	}
	return NULL;
}

/* Caller holds cdv_msvdx_lock after packet/reference validation. Firmware has
 * completed the previous picture. Drain the MMU before publishing new owned
 * NV12 and colocated pages, then invalidate using the qualified boot sequence.
 */
static int cdv_bind_output(struct drm_device *dev, void __iomem *regs,
			   struct cdv_engine_context *engine, u32 slot,
			   struct cdv_surface_storage *storage)
{
	struct cdv_surface_storage *old = engine->surfaces[slot];
	u32 i, value,
		aux = (CDV_H264_COLOCATED_BASE - CDV_H264_ARENA_VA +
		       slot * CDV_H264_COLOCATED_BYTES) /
		      PAGE_SIZE;
	int ret;

	if (old == storage)
		return 0;
	/* Firmware leaves INVALDC (bit3) asserted after completion. The pinned
	 * Cedarview definition identifies it as invalidation, not MMU bypass.
	 */
	if (!engine->mapped || (readl(regs + MSVDX_MMU_CONTROL) & ~8U))
		return -EUCLEAN;
	for (i = 0; i < ARRAY_SIZE(engine->saved_directory); i++)
		if (readl(regs + MSVDX_MMU_DIRECTORY + i * 4) !=
		    lower_32_bits(engine->directory_dma))
			return -EUCLEAN;
	writel(2, regs + MSVDX_MMU_CONTROL);
	ret = readl_poll_timeout(regs + MSVDX_MMU_REQUESTS, value, !(value & 0xff), 10, 10000);
	if (ret) {
		writel(0, regs + MSVDX_MMU_CONTROL);
		return ret;
	}
	kref_get(&storage->refs);
	for (i = 0; i < CDV_SURFACE_SIZE / PAGE_SIZE; i++) {
		struct cdv_dma_page page = storage->pages[i];

		if (slot) {
			u32 index = (slot - 1) * (CDV_SURFACE_SIZE / PAGE_SIZE) + i;

			engine->reference_pages[index] = page;
			engine->reference_table[index] = cpu_to_le32(lower_32_bits(page.dma) | 1);
		} else {
			u32 index = CDV_H264_OUTPUT_OFFSET / PAGE_SIZE + i;

			engine->arena[index] = page;
			engine->table[16 + index] = cpu_to_le32(lower_32_bits(page.dma) | 1);
		}
	}
	for (i = 0; i < CDV_H264_COLOCATED_BYTES / PAGE_SIZE; i++) {
		struct cdv_dma_page page = storage->pages[CDV_SURFACE_SIZE / PAGE_SIZE + i];

		engine->arena[aux + i] = page;
		engine->table[16 + aux + i] = cpu_to_le32(lower_32_bits(page.dma) | 1);
	}
	engine->surfaces[slot] = storage;
	/* Publish page bindings before the hardware page walker resumes. */
	wmb();
	drm_clflush_virt_range(engine->table, CDV_COMMAND_TABLE_BYTES);
	drm_clflush_virt_range(engine->reference_table, CDV_REFERENCE_TABLE_BYTES);
	writel(8, regs + MSVDX_MMU_CONTROL);
	readl(regs + MSVDX_MMU_CONTROL);
	writel(0, regs + MSVDX_MMU_CONTROL);
	readl(regs + MSVDX_MMU_CONTROL);
	kref_put(&old->refs, cdv_storage_destroy);
	surface_bindings++;
	return 0;
}

/* Field positions and patch locations are generated from the pinned public
 * DE2 register definitions/builder. No user register word is accepted.
 */
static u32 cdv_patch_h264_runtime(__le32 *command, const struct cedar_h264_config *c)
{
	u32 profile = c->profile_idc == 66 ? 0 : c->profile_idc == 77 ? 1 : 2;
	u32 flags = c->config_flags, value, pps;

#define FIELD(name, v) (((u32)(v) << CDV_H264_RT_##name##_SHIFT) & CDV_H264_RT_##name##_MASK)

	value = le32_to_cpu(command[CDV_H264_RT_FE_SPS]);
	value &= ~(CDV_H264_RT_SPS_WIDTH_MASK | CDV_H264_RT_SPS_DIRECT_MASK |
		   CDV_H264_RT_SPS_PROFILE_MASK | CDV_H264_RT_SPS_DEFAULT_MASK);
	value |= FIELD(SPS_WIDTH, c->width / 16 - 1) |
		 FIELD(SPS_DIRECT, !!(flags & CEDAR_CONFIG_DIRECT8)) |
		 FIELD(SPS_PROFILE, profile == 2 ? 3 : profile) | FIELD(SPS_DEFAULT, profile == 0);
	command[CDV_H264_RT_BE_SPS] = cpu_to_le32(value);
	command[CDV_H264_RT_FE_SPS] = command[CDV_H264_RT_BE_SPS];
	pps = FIELD(PPS_CABAC, !!(flags & CEDAR_CONFIG_CABAC)) |
	      FIELD(PPS_CONSTRAINED, !!(flags & CEDAR_CONFIG_CONSTRAINED)) |
	      FIELD(PPS_TRANSFORM, !!(flags & CEDAR_CONFIG_TRANSFORM8)) |
	      FIELD(PPS_CHROMA, c->chroma_qp_offset) |
	      FIELD(PPS_SECOND_CHROMA, c->second_chroma_qp_offset) |
	      FIELD(PPS_WEIGHT_B, flags >> CEDAR_CONFIG_WEIGHT_B_SHIFT);
	command[CDV_H264_RT_BE_PPS] = cpu_to_le32(pps);
	command[CDV_H264_RT_FE_PPS] = command[CDV_H264_RT_BE_PPS];
	value = le32_to_cpu(command[CDV_H264_PATCH_FE_PIC0]);
	value &= ~(CDV_H264_RT_PIC_SIZE_MASK | CDV_H264_RT_PIC_HEIGHT_MASK);
	value |= FIELD(PIC_SIZE, c->width * c->height / 256 - 1) |
		 FIELD(PIC_HEIGHT, c->height / 16 - 1);
	command[CDV_H264_PATCH_BE_PIC0] = cpu_to_le32(value);
	command[CDV_H264_PATCH_FE_PIC0] = command[CDV_H264_PATCH_BE_PIC0];
	value = le32_to_cpu(command[CDV_H264_PATCH_FE_SLICE0]);
	value &= ~(CDV_H264_RT_SLICE_DEBLOCK_MASK | CDV_H264_RT_SLICE_ALPHA_MASK |
		   CDV_H264_RT_SLICE_BETA_MASK);
	value |= FIELD(SLICE_DEBLOCK, c->disable_deblock) | FIELD(SLICE_ALPHA, c->alpha) |
		 FIELD(SLICE_BETA, c->beta);
	command[CDV_H264_PATCH_BE_SLICE0] = cpu_to_le32(value);
	command[CDV_H264_PATCH_FE_SLICE0] = command[CDV_H264_PATCH_BE_SLICE0];
	value = le32_to_cpu(command[1]);
	value &= ~(CDV_H264_RT_PARAM_CONSTRAINED_MASK | CDV_H264_RT_PARAM_MODE_MASK |
		   CDV_H264_RT_PARAM_DEBLOCK_MASK | CDV_H264_RT_PARAM_ALPHA_MASK |
		   CDV_H264_RT_PARAM_BETA_MASK);
	value |= FIELD(PARAM_CONSTRAINED, !!(flags & CEDAR_CONFIG_CONSTRAINED)) |
		 FIELD(PARAM_MODE, (!!(flags & CEDAR_CONFIG_WEIGHT_P)) |
					   (((flags >> CEDAR_CONFIG_WEIGHT_B_SHIFT) & 3) << 1)) |
		 FIELD(PARAM_DEBLOCK, c->disable_deblock) | FIELD(PARAM_ALPHA, c->alpha) |
		 FIELD(PARAM_BETA, c->beta);
	command[CDV_H264_PATCH_SLICE_PARAMS] = cpu_to_le32(value);
	command[1] = command[CDV_H264_PATCH_SLICE_PARAMS];
	command[4] = cpu_to_le32(((c->height / 16 - 1) << 8) | (c->width / 16 - 1));
	value = le32_to_cpu(command[CDV_H264_RT_FE_ENTROPY]);
	value = (value & ~CDV_H264_RT_ENTROPY_PROFILE_MASK) | FIELD(ENTROPY_PROFILE, profile);
	command[CDV_H264_RT_BE_ENTROPY] = cpu_to_le32(value);
	command[CDV_H264_RT_FE_ENTROPY] = command[CDV_H264_RT_BE_ENTROPY];
	value = FIELD(SIZE_WIDTH, c->width - 1) | FIELD(SIZE_HEIGHT, c->height - 1);
	command[CDV_H264_RT_CODED] = cpu_to_le32(value);
	command[CDV_H264_RT_DISPLAY] = command[CDV_H264_RT_CODED];
	value = le32_to_cpu(command[CDV_H264_RT_OPERATING]);
	value = (value & ~CDV_H264_RT_OPERATING_PROFILE_MASK) | FIELD(OPERATING_PROFILE, profile);
	command[CDV_H264_RT_OPERATING] = cpu_to_le32(value);
#undef FIELD
	return value;
}

static void cdv_patch_h264_weights(__le32 *command, const struct cedar_h264_weights *w, u32 index)
{
#define FIELD(name, v) (((u32)(v) << CDV_H264_RT_##name##_SHIFT) & CDV_H264_RT_##name##_MASK)
	command[CDV_H264_RT_WEIGHTS_A + index] =
		cpu_to_le32(FIELD(Y_WEIGHT, w->luma_weight) | FIELD(CB_WEIGHT, w->cb_weight) |
			    FIELD(CR_WEIGHT, w->cr_weight));
	command[CDV_H264_RT_OFFSETS_A + index] =
		cpu_to_le32(FIELD(Y_OFFSET, w->luma_offset) | FIELD(CB_OFFSET, w->cb_offset) |
			    FIELD(CR_OFFSET, w->cr_offset));
	command[CDV_H264_RT_DENOM] =
		cpu_to_le32(FIELD(Y_DENOM, w->luma_denom) | FIELD(C_DENOM, w->chroma_denom));
#undef FIELD
}

static void cdv_patch_h264_slice(__le32 *command, const struct cdv_job *request)
{
	u32 x = request->first_mb % (request->width / 16),
	    y = request->first_mb / (request->width / 16);
	u32 value;

#define FIELD(name, v) (((u32)(v) << CDV_H264_RT_##name##_SHIFT) & CDV_H264_RT_##name##_MASK)
	command[4] = cpu_to_le32((((y << 8) | x) << 16) |
				 (((request->height / 16 - 1) << 8) | (request->width / 16 - 1)));
	value = le32_to_cpu(command[CDV_H264_PATCH_FE_SLICE0]);
	value = (value & ~CDV_H264_RT_SLICE_INDEX_MASK) | FIELD(SLICE_INDEX, request->slice_index);
	command[CDV_H264_PATCH_BE_SLICE0] = cpu_to_le32(value);
	command[CDV_H264_PATCH_FE_SLICE0] = command[CDV_H264_PATCH_BE_SLICE0];
	value = le32_to_cpu(command[CDV_H264_PATCH_FE_SLICE1]);
	value = (value & ~(CDV_H264_RT_MB_X_MASK | CDV_H264_RT_MB_Y_MASK)) | FIELD(MB_X, x) |
		FIELD(MB_Y, y);
	command[CDV_H264_PATCH_BE_SLICE1] = cpu_to_le32(value);
	command[CDV_H264_PATCH_FE_SLICE1] = command[CDV_H264_PATCH_BE_SLICE1];
	command[CDV_H264_RT_FE_SLICE2] = cpu_to_le32(FIELD(MB_ABS, request->first_mb));
#undef FIELD
}

static int cdv_engine_submit_picture(struct drm_device *dev, void __iomem *regs,
				     struct cdv_engine_context *engine, struct cdv_job *job,
				     u32 frame)
{
	u32 i, slice;
	int ret = 0;
	__le32 *command = engine->command;

	const struct cdv_picture *picture = &job->pictures[frame];
	u32 output_slot = picture->output;
	struct cdv_dma_page *output_pages = output_slot ? engine->reference_pages : engine->arena;
	u32 output_base = output_slot ? (output_slot - 1) * CDV_SURFACE_SIZE :
					CDV_H264_OUTPUT_OFFSET;
	u32 width = job->width;
	u32 height = job->height;
	u32 plane_height = CDV_H264_HEIGHT;
	u32 frame_bytes = width * height * 3 / 2;
	u32 addresses[CDV_H264_SURFACES];

	addresses[0] = CDV_H264_ARENA_VA + CDV_H264_OUTPUT_OFFSET;
	for (i = 1; i < CDV_H264_SURFACES; i++)
		addresses[i] = CDV_REFERENCES_VA + (i - 1) * CDV_SURFACE_SIZE;

	for (i = output_base / PAGE_SIZE; i < (output_base + CDV_SURFACE_SIZE) / PAGE_SIZE; ++i) {
		/* Initialize output storage before the device writes the picture. */
		memset(output_pages[i].cpu, 0xa5, PAGE_SIZE);
		drm_clflush_virt_range(output_pages[i].cpu, PAGE_SIZE);
	}
	for (slice = 0; slice < (job && job->children ? job->slice_count : CDV_H264_SLICE_COUNT);
	     ++slice) {
		struct cdv_job *request = job && job->children ? job->children[slice] : job;
		u32 request_frame = job && job->children ? 0 : frame;
		const struct cdv_picture *slice_picture = &request->pictures[request_frame];
		u32 template_slice = job && job->children ? 0 : slice;
		u32 template_base = template_slice * CDV_H264_COMMAND_WORDS;
		u32 slot =
			request_frame * CDV_H264_SLICE_COUNT + (job && job->children ? 0 : slice);
		u32 bytes = request->lengths[slot];
		u32 operating_mode = CDV_H264_OPERATING_MODE;
		const u8 *payload = request->packet + request->offsets[slot];
		/* Full lists expand two chunks only after all template-indexed
		 * patching. Restore the template before the next submission.
		 */
		command[0xc00 / 4 + 1] = cpu_to_le32(0x80040000 | CDV_H264_COMMAND_WORDS);

		for (i = 0; i < CDV_H264_COMMAND_WORDS; ++i)
			command[i] = cpu_to_le32(cdv_h264_commands[template_base + i]);
		if (request && request->version >= 7)
			operating_mode = cdv_patch_h264_runtime(command, &request->config);
		if (request && request->version >= 8 && slice_picture->type == 1 &&
		    (request->config.config_flags & CEDAR_CONFIG_WEIGHT_P)) {
			cdv_patch_h264_weights(command, &request->weights, 0);
			if (((request->version == 9 && request->p_refs.count == 2) ||
			     (request->version == 11 && request->refs.l0_count == 2) ||
			     (request->version >= 12 && request->dpb.l0_count >= 2)))
				cdv_patch_h264_weights(command, &request->weights1, 1);
			if (request->version >= 12 && request->version != 14) {
				u32 w;

				for (w = 2; w < request->dpb.l0_count; w++)
					cdv_patch_h264_weights(command,
							       &request->weights_extra[w - 2], w);
			}
			if (request->version == 14) {
				u32 w;

				for (w = 0; w < request->full.l0_count; w++)
					cdv_patch_h264_weights(command, &request->full_weights[w],
							       w);
			}
		}
		command[CDV_H264_PATCH_BITSTREAM_SIZE] = cpu_to_le32(bytes);
		if (request && request->version >= 3) {
			u32 value = le32_to_cpu(command[CDV_H264_PATCH_FE_SLICE1]);

			value = (value & ~CDV_H264_QP_MASK) |
				(request->h264_qps[request_frame] << CDV_H264_QP_SHIFT);
			command[CDV_H264_PATCH_FE_SLICE1] = cpu_to_le32(value);
			command[CDV_H264_PATCH_BE_SLICE1] = cpu_to_le32(value);
			command[CDV_H264_PATCH_SR_OFFSET] =
				cpu_to_le32(request->h264_offsets[request_frame]);
		}
		if (request && request->version >= 4) {
			u32 code = slice_picture->type == 5 || slice_picture->type == 6 ?
					   0 :
					   slice_picture->type;
			u32 value = le32_to_cpu(command[CDV_H264_PATCH_FE_SLICE0]);
			u32 ref = addresses[code ? slice_picture->forward : slice_picture->output];

			value = (value &
				 ~(CDV_H264_SLICETYPE_MASK | CDV_H264_CABAC_INIT_IDC_MASK)) |
				(code << CDV_H264_SLICETYPE_SHIFT) |
				(request->h264_cabac_init[request_frame]
				 << CDV_H264_CABAC_INIT_IDC_SHIFT);
			command[CDV_H264_PATCH_BE_SLICE0] = cpu_to_le32(value);
			command[CDV_H264_PATCH_FE_SLICE0] = command[CDV_H264_PATCH_BE_SLICE0];
			value = (le32_to_cpu(command[1]) & ~CDV_H264_SLICE_CODE_MASK) |
				(code << CDV_H264_SLICE_CODE_SHIFT);
			command[CDV_H264_PATCH_SLICE_PARAMS] = cpu_to_le32(value);
			command[1] = command[CDV_H264_PATCH_SLICE_PARAMS];
			command[CDV_H264_PATCH_OUTPUT] =
				cpu_to_le32(addresses[slice_picture->output]);
			value = addresses[slice_picture->output] + CDV_H264_STRIDE * plane_height;
			command[CDV_H264_PATCH_OUTPUT + 1] = cpu_to_le32(value);
			command[CDV_H264_PATCH_REFERENCES] = cpu_to_le32(ref);
			command[CDV_H264_PATCH_REFERENCES + 1] =
				cpu_to_le32(ref + CDV_H264_STRIDE * plane_height);
		}
		if (request && request->version >= 5) {
			u32 value = le32_to_cpu(command[CDV_H264_PATCH_FE_PIC0]);
			u32 back = slice_picture->type == 2 ? slice_picture->backward :
							      slice_picture->output;
			u32 p;

			value = (value & ~CDV_H264_REFERENCE_MASK) |
				((request->version == 14 ? request->full.reference :
				  request->version == 13 ? request->dpb.reserved[1] :
							   slice_picture->type != 2)
				 << CDV_H264_REFERENCE_SHIFT);
			command[CDV_H264_PATCH_BE_PIC0] = cpu_to_le32(value);
			command[CDV_H264_PATCH_FE_PIC0] = command[CDV_H264_PATCH_BE_PIC0];
			value = le32_to_cpu(command[CDV_H264_PATCH_FE_SLICE0]);
			value = (value & ~CDV_H264_DIRECT_MASK) |
				(request->h264_direct[request_frame] << CDV_H264_DIRECT_SHIFT);
			command[CDV_H264_PATCH_BE_SLICE0] = cpu_to_le32(value);
			command[CDV_H264_PATCH_FE_SLICE0] = command[CDV_H264_PATCH_BE_SLICE0];
			command[CDV_H264_PATCH_CURRENT_COL] =
				cpu_to_le32(CDV_H264_COLOCATED_BASE +
					    slice_picture->output * CDV_H264_COLOCATED_BYTES);
			value = CDV_H264_COLOCATED_BASE + back * CDV_H264_COLOCATED_BYTES;
			command[CDV_H264_PATCH_BACKWARD_COL] = cpu_to_le32(value);
			command[CDV_H264_PATCH_REFERENCES + 2] = cpu_to_le32(addresses[back]);
			command[CDV_H264_PATCH_REFERENCES + 3] =
				cpu_to_le32(addresses[back] + CDV_H264_STRIDE * plane_height);
			for (p = 0; p < 3; ++p)
				command[CDV_H264_PATCH_POC + p * 2 + 1] =
					cpu_to_le32(request->h264_pocs[request_frame][p]);
			command[CDV_H264_PATCH_POC + p * 2] =
				command[CDV_H264_PATCH_POC + p * 2 + 1];
		}
		if (request && request->version == 9) {
			u32 value = le32_to_cpu(command[CDV_H264_PATCH_FE_SLICE1]);
			u32 second = request->p_refs.count == 2 ? request->p_refs.slots[1] :
								  slice_picture->output;

			value = (value & ~CDV_H264_RT_REF_COUNT_MASK) |
				((request->p_refs.count - 1) << CDV_H264_RT_REF_COUNT_SHIFT);
			command[CDV_H264_PATCH_BE_SLICE1] = cpu_to_le32(value);
			command[CDV_H264_PATCH_FE_SLICE1] = command[CDV_H264_PATCH_BE_SLICE1];
			command[CDV_H264_RT_LIST0] =
				cpu_to_le32(request->p_refs.count == 2 ? 0xffff0100 : 0xffffff00);
			command[CDV_H264_PATCH_REFERENCES + 2] = cpu_to_le32(addresses[second]);
			command[CDV_H264_PATCH_REFERENCES + 3] =
				cpu_to_le32(addresses[second] + CDV_H264_STRIDE * plane_height);
		}
		if (request && (request->version == 11 || request->version >= 12)) {
			const struct cedar_h264_refs *r = &request->refs;
			const struct cedar_h264_dpb *d = &request->dpb;
			bool expanded = request->version >= 12;
			u32 l0_count = request->version == 14 ? request->full.l0_count :
				       expanded		      ? d->l0_count :
								r->l0_count;
			u32 l1_count = request->version == 14 ? request->full.l1_count :
				       expanded		      ? d->l1_count :
								r->l1_count;
			const u32 *l0 = request->version == 14 ? request->full.l0_slots :
					expanded	       ? d->l0_slots :
								 r->l0_slots;
			const u32 *l1 = request->version == 14 ? request->full.l1_slots :
					expanded	       ? d->l1_slots :
								 r->l1_slots;
			u32 p, list0 = U32_MAX, list1 = U32_MAX, inverse[2] = { U32_MAX, U32_MAX };
			u32 value = le32_to_cpu(command[CDV_H264_PATCH_FE_SLICE1]);

			value = (value &
				 ~(CDV_H264_RT_REF_COUNT_MASK | CDV_H264_RT_REF_COUNT1_MASK)) |
				((l0_count ? l0_count - 1 : 0) << CDV_H264_RT_REF_COUNT_SHIFT) |
				((l1_count ? l1_count - 1 : 0) << CDV_H264_RT_REF_COUNT1_SHIFT);
			command[CDV_H264_PATCH_BE_SLICE1] = cpu_to_le32(value);
			command[CDV_H264_PATCH_FE_SLICE1] = command[CDV_H264_PATCH_BE_SLICE1];
			for (p = 0; p < l0_count; p++) {
				if (p < 4)
					list0 = (list0 & ~(0xffU << (8 * p))) | (l0[p] << (8 * p));
				if (((inverse[l0[p] / 4] >> (8 * (l0[p] % 4))) & 255) == 255)
					inverse[l0[p] / 4] = (inverse[l0[p] / 4] &
							      ~(0xffU << (8 * (l0[p] % 4)))) |
							     (p << (8 * (l0[p] % 4)));
			}
			for (p = 0; p < min(l1_count, 4U); p++)
				list1 = (list1 & ~(0xffU << (8 * p))) | (l1[p] << (8 * p));
			command[CDV_H264_RT_LIST0] = cpu_to_le32(list0);
			command[CDV_H264_RT_LIST1] = cpu_to_le32(list1);
			command[CDV_H264_RT_INVERSE] = cpu_to_le32(inverse[0]);
			command[CDV_H264_RT_INVERSE + 1] = cpu_to_le32(inverse[1]);
			for (p = 0; p < (expanded ? CDV_H264_SURFACES : 3); p++) {
				command[CDV_H264_PATCH_REFERENCES + p * 2] =
					cpu_to_le32(addresses[p]);
				command[CDV_H264_PATCH_REFERENCES + p * 2 + 1] =
					cpu_to_le32(addresses[p] + CDV_H264_STRIDE * plane_height);
				command[CDV_H264_PATCH_POC + 3 + p * 2] =
					cpu_to_le32(request->dpb_pocs[p]);
				command[CDV_H264_PATCH_POC + 2 + p * 2] =
					command[CDV_H264_PATCH_POC + 3 + p * 2];
			}
			if (request->version == 14) {
				/* Translate signed 17-bit hardware FOCs together;
				 * the validated DPB retains full software POCs.
				 */
				u32 current_poc = request->h264_pocs[0][0];

				command[CDV_H264_PATCH_POC + 1] = 0;
				command[CDV_H264_PATCH_POC] = command[CDV_H264_PATCH_POC + 1];
				for (p = 0; p < CDV_H264_SURFACES; p++) {
					s64 delta =
						request->full.retained_mask & BIT(p) ?
							(s64)request->dpb_pocs[p] - current_poc :
							0;

					command[CDV_H264_PATCH_POC + 3 + p * 2] =
						cpu_to_le32((u32)delta & 0x1ffffU);
					command[CDV_H264_PATCH_POC + 2 + p * 2] =
						command[CDV_H264_PATCH_POC + 3 + p * 2];
				}
			}
		}
		if (job && job->children)
			cdv_patch_h264_slice(command, request);
		if (request && request->version == 14) {
			u32 descriptor[8], index, chunk, w;

			if (!engine->bitstream)
				return -EINVAL;
			cdv_arena_write(engine->bitstream, 0, payload, bytes);
			for (index = 0;
			     (chunk = cedar_h264_dma_descriptor(descriptor, CDV_COMMAND_VA,
								CDV_BITSTREAM_VA, bytes, index));
			     index++)
				for (w = 0; w < 8; w++)
					command[cedar_h264_dma_offset(index) / 4 + w] =
						cpu_to_le32(descriptor[w]);
			for (i = 0; i < DIV_ROUND_UP(bytes, PAGE_SIZE); i++)
				drm_clflush_virt_range(engine->bitstream[i].cpu, PAGE_SIZE);
		} else {
			cdv_arena_write(engine->arena, PAGE_SIZE, payload, bytes);
			command[0xc60 / 4 + 1] = cpu_to_le32(0x80040000 | bytes);
		}
		if (request && request->version == 14) {
			u32 list, entry, position, value;
			/* The pinned DE2 builder emits consecutive four-byte
			 * list entries. Expand one-word LIST0 and the final
			 * LIST1 word in the BIN chunk to eight words each.
			 * Higher insertion first preserves original indices.
			 */
			static_assert((CDV_H264_COMMAND_WORDS + 14) * 4 < 0xc00);
			for (list = 0; list < 2; list++) {
				const u32 *refs = list ? request->full.l1_slots :
							 request->full.l0_slots;

				position = list ? CDV_H264_RT_LIST1 : CDV_H264_RT_LIST0;
				memmove(command + position + 8, command + position + 1,
					(CDV_H264_COMMAND_WORDS + (list ? 7 : 0) - position - 1) *
						sizeof(__le32));
				for (entry = 0; entry < 8; entry++)
					command[position + entry] =
						cpu_to_le32((refs[entry * 4] & 255) |
							    ((refs[entry * 4 + 1] & 255) << 8) |
							    ((refs[entry * 4 + 2] & 255) << 16) |
							    ((refs[entry * 4 + 3] & 255) << 24));
			}
			/* LIST0 moved by seven words when BIN was expanded. */
			position = CDV_H264_RT_LIST0 + 7 - 1;
			value = le32_to_cpu(command[position]);
			command[position] = cpu_to_le32((value & ~0xff0000U) | (8U << 16));
			position = CDV_H264_RT_INVERSE - 3;
			value = le32_to_cpu(command[position]);
			command[position] = cpu_to_le32((value & ~0xff0000U) | (18U << 16));
			command[0xc00 / 4 + 1] =
				cpu_to_le32(0x80040000 | (CDV_H264_COMMAND_WORDS + 14));
		}
		/* Drain CPU writes before firmware consumes the command and input. */
		wmb();
		drm_clflush_virt_range(command, PAGE_SIZE);
		if (!request || request->version != 14)
			for (i = 1; i <= CDV_H264_BITSTREAM_PAGES; ++i)
				drm_clflush_virt_range(engine->arena[i].cpu, PAGE_SIZE);
		ret = (job &&
		       ((job->flags & 1) || (job->children && (job->flags & 2) && slice == 1))) ?
			      -ECANCELED :
			      cdv_submit_command(dev, regs, engine->directory_dma,
						 engine->next_sequence++, operating_mode,
						 !request || !request->slice_index,
						 CDV_H264_COMMAND_WORDS +
							 (request && request->version == 14 ? 14 :
											      0));
		writel(BIT(14), regs + 0x060c);
		if (!ret) {
			engine->command_completed = true;
			if (bytes > 65535)
				large_slices++;
			max_slice_bytes_seen = max(max_slice_bytes_seen, bytes);
		}
		if (ret)
			break;
	}
	if (job && ret && (job->children || !(job->flags & 1)))
		return ret; /* Failed/partial pictures are never exposed or committed. */
	if (job) {
		if (job->output)
			cdv_snapshot_at(output_pages, output_base,
					job->output + picture->display * frame_bytes, width,
					height);
		if (job->output && (job->version == 1 || frame + 1 == job->frames))
			job->available = (frame + 1) * frame_bytes;
		dev_dbg(dev->dev,
			"cedar-msvdx: stage=userspace_picture result=%d frame=%u bytes=%u\n", ret,
			frame, frame_bytes);
		if (job->version == 2 || job->version >= 5)
			dev_dbg(dev->dev,
				"cedar-msvdx: picture type=%u output=%u forward=%u backward=%u display=%u\n",
				picture->type, picture->output, picture->forward, picture->backward,
				picture->display);
	}
	if (!ret) {
		completed_frames++;
		if (job && frame + 1 == job->frames)
			job->hardware_complete = true;
	}
	if (ret)
		return ret;

	return ret;
}

static int cdv_engine_submit(struct drm_device *dev, void __iomem *regs,
			     struct cdv_engine_context *engine, struct cdv_job *job)
{
	u32 frame;
	int ret = 0;

	if (job->bound_output) {
		ret = cdv_bind_output(dev, regs, engine, job->pictures[0].output,
				      job->bound_output);
		if (ret)
			return ret;
	}
	for (frame = 0; frame < job->frames; frame++) {
		ret = cdv_engine_submit_picture(dev, regs, engine, job, frame);
		if (ret)
			break;
	}
	if (ret)
		dev_warn(dev->dev, "cedar-msvdx: picture batch failed: %d (%u/%u completed)\n", ret,
			 frame, job->frames);
	return ret;
}

/* Abort only the observed first-command, unconsumed, stopped-producer failure
 * with one stuck request under our retained independent mapping.
 * Never reset shared SGX/display state.
 * No backing memory is released unless reset completion, empty requests,
 * stopped DMA and invalid hardware directories are all observed afterwards.
 */
static int cdv_abort_owned_engine(struct drm_device *dev, void __iomem *regs,
				  struct cdv_engine_context *engine)
{
	u32 value, i;
	int ret;

	if (!engine->mapped || engine->command_completed || engine->next_sequence != 2 ||
	    readl(regs + 0x2ff4) != 0 || readl(regs + MSVDX_MMU_CONTROL) != 2 ||
	    (readl(regs + MTX_ENABLE) & 1) || (readl(regs + MTX_SYSC_CDMAC) & BIT(16)) ||
	    (readl(regs + MSVDX_MMU_REQUESTS) & 255) != 1)
		return -EBUSY;
	for (i = 0; i < 4; i++)
		if (readl(regs + MSVDX_MMU_DIRECTORY + i * 4) !=
			    lower_32_bits(engine->directory_dma) ||
		    (readl(regs + 0x0504 + i * 0x20) & BIT(16)))
			return -EBUSY;

	dev_warn(dev->dev,
		 "cedar-msvdx: aborting stopped owned decoder with one stuck MMU request\n");
	writel(0x7f, regs + CDV_MSVDX_CLOCK);
	writel(readl(regs + 0x086c) | BIT(27), regs + 0x086c);
	writel(~0x100U, regs + 0x0600);
	readl(regs + 0x0600);
	writel(0, regs + 0x0600);
	writel(0x100, regs + 0x0600);
	ret = readl_poll_timeout(regs + 0x0600, value, !(value & 0x100), 10, 100000);
	writel(0x7f, regs + CDV_MSVDX_CLOCK);
	writel(2, regs + MSVDX_MMU_CONTROL);
	readl(regs + MSVDX_MMU_CONTROL);
	if (!ret)
		ret = readl_poll_timeout(regs + MSVDX_MMU_REQUESTS, value, !(value & 255), 10,
					 100000);
	if (!ret && ((readl(regs + MTX_ENABLE) & 1) || (readl(regs + MTX_SYSC_CDMAC) & BIT(16))))
		ret = -EBUSY;
	for (i = 0; !ret && i < 4; i++)
		if (readl(regs + MSVDX_MMU_DIRECTORY + i * 4) ||
		    (readl(regs + 0x0504 + i * 0x20) & BIT(16)))
			ret = -EBUSY;
	if (ret) {
		forced_reset_failures++;
	} else {
		forced_resets++;
		writel(0, regs + MSVDX_HOST_IRQ_ENABLE);
		writel(U32_MAX, regs + 0x060c);
	}
	dev_info(dev->dev, "cedar-msvdx: stage=owned_abort result=%d requests=%08x control=%08x\n",
		 ret, readl(regs + MSVDX_MMU_REQUESTS), readl(regs + 0x0600));
	return ret;
}

static int cdv_engine_destroy(struct drm_device *dev, void __iomem *regs,
			      struct cdv_engine_context *engine)
{
	u32 value, i;
	int quiesce;

	if (engine->mapped) {
		writel(0x7f, regs + CDV_MSVDX_CLOCK);
		writel(1, regs + MTX_SOFT_RESET);
		writel(2, regs + MSVDX_MMU_CONTROL);
		quiesce = readl_poll_timeout(regs + MSVDX_MMU_REQUESTS, value, !(value & 0xff), 10,
					     100000);
		if (!quiesce)
			quiesce = cdv_reset_decoder(regs);
		if (quiesce)
			quiesce = cdv_abort_owned_engine(dev, regs, engine);
		if (quiesce) {
			/* Retain the invalid directory rather than free memory still in use. */
			dev_err(dev->dev,
				"cedar-msvdx: decoder quarantined; retained owned DMA pages; reboot required\n");
			/* Independent DMA-BUFs may release their last module reference
			 * after producer closure. A failed engine proof must outlive them.
			 */
			{
				cdv_quarantine_engine = engine;
				cdv_quarantine_regs = regs;
				drm_dev_get(dev);
				__module_get(THIS_MODULE);
			}
			return -EUCLEAN;
		}
		for (i = 0; i < ARRAY_SIZE(engine->saved_directory); ++i)
			writel(engine->saved_directory[i], regs + MSVDX_MMU_DIRECTORY + i * 4);
		writel(engine->saved_bank, regs + MSVDX_MMU_BANK);
		writel(engine->saved_control, regs + MSVDX_MMU_CONTROL);
		writel(U32_MAX, regs + 0x060c);
		writel(engine->saved_irq, regs + MSVDX_HOST_IRQ_ENABLE);
	}
	kfree(engine->reference_pages);
	for (i = 0; i < engine->bitstream_allocated; i++) {
		cdv_surface_page_free(dev, &engine->bitstream[i]);
		atomic_dec(&bitstream_pages);
	}
	kfree(engine->bitstream);
	if (engine->bitstream_table)
		dma_free_wc(dev->dev, PAGE_SIZE, engine->bitstream_table,
			    engine->bitstream_table_dma);
	if (engine->reference_table)
		dma_free_wc(dev->dev, CDV_REFERENCE_TABLE_BYTES, engine->reference_table,
			    engine->reference_table_dma);
	for (i = 0; i < engine->arena_allocated; ++i)
		if (!cdv_arena_storage_page(engine, i))
			cdv_surface_page_free(dev, &engine->arena[i]);
	kfree(engine->arena);
	for (i = 0; i < ARRAY_SIZE(engine->surfaces); i++)
		if (engine->surfaces[i])
			kref_put(&engine->surfaces[i]->refs, cdv_storage_destroy);
	if (engine->firmware_buffer)
		dma_free_wc(dev->dev, MTX_MEMORY_LIMIT, engine->firmware_buffer,
			    engine->firmware_dma);
	for (i = 0; i < engine->rendec_allocated; ++i)
		dma_free_wc(dev->dev, PAGE_SIZE, engine->rendec_pages[i].cpu,
			    engine->rendec_pages[i].dma);
	kfree(engine->rendec_pages);
	if (engine->rendec_tables)
		dma_free_wc(dev->dev, 2 * PAGE_SIZE, engine->rendec_tables,
			    engine->rendec_tables_dma);
	if (engine->command)
		dma_free_wc(dev->dev, PAGE_SIZE, engine->command, engine->command_dma);
	if (engine->table)
		dma_free_wc(dev->dev, CDV_COMMAND_TABLE_BYTES, engine->table, engine->table_dma);
	if (engine->directory)
		dma_free_wc(dev->dev, PAGE_SIZE, engine->directory, engine->directory_dma);
	kfree(engine);
	atomic_dec(&engine_contexts);
	return 0;
}

/* Reuse the same owned mappings and immutable firmware image after sleep.
 * Reference images and colocated motion data are deliberately preserved.
 */
static int cdv_engine_start(struct drm_device *dev, void __iomem *regs,
			    struct cdv_engine_context *engine, bool save_state)
{
	u32 value, i;
	int ret;

	/* These CPU mappings are WC. dma_wmb() alone is a compiler barrier on
	 * x86; drain WC stores before allowing the hardware page walker/DMA.
	 */
	wmb();
	/* The legacy Cedarview MMU explicitly flushes CPU page-table aliases. */
	drm_clflush_virt_range(engine->directory, PAGE_SIZE);
	if (engine->table)
		drm_clflush_virt_range(engine->table, CDV_COMMAND_TABLE_BYTES);
	if (engine->rendec_tables)
		drm_clflush_virt_range(engine->rendec_tables, 2 * PAGE_SIZE);
	if (engine->reference_table)
		drm_clflush_virt_range(engine->reference_table, CDV_REFERENCE_TABLE_BYTES);
	if (engine->command)
		drm_clflush_virt_range(engine->command, PAGE_SIZE);
	if (engine->firmware_buffer)
		drm_clflush_virt_range(engine->firmware_buffer, MTX_MEMORY_LIMIT);
	if (engine->arena)
		cdv_flush_arena(engine->arena);
	if (engine->table)
		dev_info(dev->dev,
			 "cedar-msvdx: published pde4=%08x pde8=%08x pte0=%08x pte1=%08x rendec_pte0=%08x\n",
			 le32_to_cpu(engine->directory[4]), le32_to_cpu(engine->directory[8]),
			 le32_to_cpu(engine->table[0]), le32_to_cpu(engine->table[1]),
			 engine->rendec_tables ? le32_to_cpu(engine->rendec_tables[0]) : 0);
	if (save_state) {
		engine->saved_control = readl(regs + MSVDX_MMU_CONTROL);
		engine->saved_bank = readl(regs + MSVDX_MMU_BANK);
		for (i = 0; i < ARRAY_SIZE(engine->saved_directory); ++i)
			engine->saved_directory[i] = readl(regs + MSVDX_MMU_DIRECTORY + i * 4);
		engine->saved_irq = readl(regs + MSVDX_HOST_IRQ_ENABLE);
	}
	writel(2, regs + MSVDX_MMU_CONTROL); /* Pause before changing page tables. */
	ret = readl_poll_timeout(regs + MSVDX_MMU_REQUESTS, value, !(value & 0xff), 10, 10000);
	if (ret) {
		writel(engine->saved_control, regs + MSVDX_MMU_CONTROL);
		return ret;
	}
	writel(0, regs + MSVDX_HOST_IRQ_ENABLE);
	writel(U32_MAX, regs + 0x060c); /* Clear decoder-local pending IRQs. */
	writel(0, regs + MSVDX_MMU_BANK);
	/* Every requestor bank is confined to the same owned decoder map. */
	for (i = 0; i < ARRAY_SIZE(engine->saved_directory); ++i)
		writel(lower_32_bits(engine->directory_dma), regs + MSVDX_MMU_DIRECTORY + i * 4);
	writel(8, regs + MSVDX_MMU_CONTROL); /* Invalidate, with bypass bits clear. */
	readl(regs + MSVDX_MMU_CONTROL);
	writel(0, regs + MSVDX_MMU_CONTROL);
	engine->mapped = true;

	ret = cdv_dma_transfer(dev, regs, CDV_COMMAND_VA + PAGE_SIZE, 0, MTX_MEMORY_LIMIT / 4,
			       engine->firmware_buffer);
	if (ret)
		return ret;
	/* Exact RENDEC sizes and initialization from the Cedarview DDK. */
	writel(CDV_RENDEC_VA, regs + 0x0874);
	writel(CDV_RENDEC_VA + CDV_RENDEC_A_SIZE, regs + 0x0878);
	writel((CDV_RENDEC_A_SIZE / PAGE_SIZE) | ((CDV_RENDEC_B_SIZE / PAGE_SIZE) << 16),
	       regs + 0x0870);
	writel(0x01050000, regs + 0x086c);
	for (i = 0; i < 6; ++i)
		writel(0x00101010, regs + 0x0950 + i * 4);
	writel(1, regs + 0x0868);
	/* psb_msvdx_init selects the host shift-register input and sets
	 * decoder watchdog comparison registers before firmware loading.
	 */
	writel((readl(regs + 0x0818) & ~0x300U) | 0x100, regs + 0x0818);
	writel(820, regs + 0x066c);
	writel(8200, regs + 0x0678);

	writel(0x014d42ab, regs + 0x2fd4); /* Firmware interface identifier. */
	writel(0, regs + 0x2fd8); /* Error trigger. */
	writel(199, regs + 0x0208); /* MTX system timer divider. */
	writel(0, regs + 0x2884); /* Extended firmware error state. */
	writel(0, regs + 0x2fdc); /* Message counter. */
	writel(0, regs + 0x2fd0); /* Firmware status. */
	writel(0, regs + MSVDX_COMMS_SIGNATURE);
	writel(0, regs + 0x2fe8); /* To-host read index. */
	writel(0, regs + 0x2fec); /* To-host write index. */
	writel(0, regs + 0x2ff4); /* To-MTX read index. */
	writel(0, regs + 0x2ffc); /* To-MTX write index. */
	writel(0x3808, regs + 0x2ff8); /* Original DDK bring-up flags. */
	writel(0x80900000, regs + 0x00f8); /* MTX PC data. */
	writel(5, regs + 0x00fc); /* MTX PC write request. */
	ret = readl_poll_timeout(regs + 0x00fc, value, value & BIT(31), 5, 10000);
	if (!ret) {
		writel(1, regs + MTX_ENABLE);
		ret = readl_poll_timeout(regs + MSVDX_COMMS_SIGNATURE, value, value == 0xa5a5a5a5,
					 10, 100000);
	}
	/* The signature publishes communication buffers before the firmware
	 * necessarily reports hardware idle. Do not race its initial hardware
	 * setup with the first render kick. Bit 0 is the published HW_IDLE bit.
	 */
	if (!ret && !(readl(regs + 0x2fd0) & 1)) {
		firmware_ready_waits++;
		ret = readl_poll_timeout(regs + 0x2fd0, value, value & 1, 10, 100000);
		if (ret)
			firmware_ready_timeouts++;
	}
	dev_info(dev->dev,
		 "cedar-msvdx: stage=firmware_boot result=%d signature=%08x irq=%08x error=%08x fw_status=%08x external_map=%s\n",
		 ret, readl(regs + MSVDX_COMMS_SIGNATURE), readl(regs + MSVDX_IRQ_STATUS),
		 readl(regs + 0x2884), readl(regs + 0x2fd0), "owned_buffers");
	if (!ret)
		firmware_boots++;
	return ret;
}

static int cdv_boot_firmware(struct drm_device *dev, void __iomem *regs,
			     const struct cdv_firmware_image *image, struct cdv_job *job,
			     struct cdv_engine_context **keep)
{
	struct cdv_engine_context *engine;
	u32 i;
	int ret, cleanup;

	if ((!job || !cdv_export_layout_valid() || !CDV_H264_RUNTIME_TEMPLATE))
		return -EOPNOTSUPP;
	if ((CDV_H264_SURFACES != 5 || !CDV_H264_COLOCATED_BYTES ||
	     (CDV_H264_COLOCATED_BASE & (PAGE_SIZE - 1)) ||
	     (CDV_H264_COLOCATED_BYTES & (PAGE_SIZE - 1))))
		return -EOPNOTSUPP;
	engine = kzalloc_obj(*engine);
	if (!engine)
		return -ENOMEM;
	atomic_inc(&engine_contexts);
	engine->next_sequence = 1;

	engine->directory =
		dma_alloc_wc(dev->dev, PAGE_SIZE, &engine->directory_dma, GFP_KERNEL | GFP_DMA32);
	if (!engine->directory) {
		ret = -ENOMEM;
		goto free_directory;
	}
	if (upper_32_bits(engine->directory_dma)) {
		ret = -ERANGE;
		goto free_directory;
	}
	memset(engine->directory, 0, PAGE_SIZE); /* All PDEs invalid: external DMA must fault. */

	engine->table = dma_alloc_wc(dev->dev, CDV_COMMAND_TABLE_BYTES, &engine->table_dma,
				     GFP_KERNEL | GFP_DMA32);
	engine->command =
		dma_alloc_wc(dev->dev, PAGE_SIZE, &engine->command_dma, GFP_KERNEL | GFP_DMA32);
	if (!engine->table || !engine->command) {
		ret = -ENOMEM;
		goto free_directory;
	}
	if (upper_32_bits(engine->table_dma + CDV_COMMAND_TABLE_BYTES - 1) ||
	    upper_32_bits(engine->command_dma)) {
		ret = -ERANGE;
		goto free_directory;
	}
	memset(engine->table, 0, CDV_COMMAND_TABLE_BYTES);
	memset(engine->command, 0, PAGE_SIZE);
	static_assert(CDV_COMMAND_VA + CDV_COMMAND_TABLE_BYTES / PAGE_SIZE * CDV_MMU_SPAN <=
		      CDV_RENDEC_VA);
	for (i = 0; i < CDV_COMMAND_TABLE_BYTES / PAGE_SIZE; i++)
		engine->directory[(CDV_COMMAND_VA >> 22) + i] =
			cpu_to_le32(lower_32_bits(engine->table_dma + i * PAGE_SIZE) | 1);
	engine->table[(CDV_COMMAND_VA >> 12) & 0x3ff] =
		cpu_to_le32(lower_32_bits(engine->command_dma) | 1);
	/* Published CTRL_ALLOC_HEADER precedes every VLD command stream. */
	engine->command[0] = cpu_to_le32(0x90000000);
	engine->command[8] = cpu_to_le32(0x60000000); /* CMD_COMPLETION. */
	dev_info(dev->dev,
		 "cedar-msvdx: mmu_map directory=%08x pde=%08x pte=%08x command_va=%08x\n",
		 lower_32_bits(engine->directory_dma),
		 le32_to_cpu(engine->directory[CDV_COMMAND_VA >> 22]),
		 le32_to_cpu(engine->table[(CDV_COMMAND_VA >> 12) & 0x3ff]), CDV_COMMAND_VA);

	u32 pages = (CDV_RENDEC_A_SIZE + CDV_RENDEC_B_SIZE) / PAGE_SIZE;

	engine->rendec_tables = dma_alloc_wc(dev->dev, 2 * PAGE_SIZE, &engine->rendec_tables_dma,
					     GFP_KERNEL | GFP_DMA32);
	engine->rendec_pages = kcalloc(pages, sizeof(*engine->rendec_pages), GFP_KERNEL);
	if (!engine->rendec_tables || !engine->rendec_pages) {
		ret = -ENOMEM;
		goto free_directory;
	}
	if (upper_32_bits(engine->rendec_tables_dma + 2 * PAGE_SIZE - 1)) {
		ret = -ERANGE;
		goto free_directory;
	}
	memset(engine->rendec_tables, 0, 2 * PAGE_SIZE);
	/* The engine sees contiguous virtual buffers, not a high-order
	 * 4 MiB physical allocation that fails on fragmented systems.
	 */
	for (i = 0; i < pages; ++i) {
		struct cdv_dma_page *page = &engine->rendec_pages[i];

		page->cpu = dma_alloc_wc(dev->dev, PAGE_SIZE, &page->dma, GFP_KERNEL | GFP_DMA32);
		if (!page->cpu) {
			ret = -ENOMEM;
			goto free_directory;
		}
		engine->rendec_allocated++;
		if (upper_32_bits(page->dma)) {
			ret = -ERANGE;
			goto free_directory;
		}
		memset(page->cpu, 0, PAGE_SIZE);
		drm_clflush_virt_range(page->cpu, PAGE_SIZE);
		engine->rendec_tables[i] = cpu_to_le32(lower_32_bits(page->dma) | 1);
	}
	for (i = 0; i < 2; ++i)
		engine->directory[(CDV_RENDEC_VA >> 22) + i] =
			cpu_to_le32(lower_32_bits(engine->rendec_tables_dma + i * PAGE_SIZE) | 1);

	engine->firmware_buffer = dma_alloc_wc(dev->dev, MTX_MEMORY_LIMIT, &engine->firmware_dma,
					       GFP_KERNEL | GFP_DMA32);
	if (!engine->firmware_buffer) {
		ret = -ENOMEM;
		goto free_directory;
	}
	if (upper_32_bits(engine->firmware_dma + MTX_MEMORY_LIMIT - 1)) {
		ret = -ERANGE;
		goto free_directory;
	}
	/* Match the DDK's complete 40 KiB RAM image and stack guard. */
	memset(engine->firmware_buffer, 0xcd, MTX_MEMORY_LIMIT);
	memcpy(engine->firmware_buffer, image->text, image->text_words * 4);
	memcpy(engine->firmware_buffer + image->data_offset, image->data, image->data_words * 4);
	put_unaligned_le32(0x10101010, engine->firmware_buffer + MTX_MEMORY_LIMIT - 4);
	for (i = 0; i < MTX_MEMORY_LIMIT / PAGE_SIZE; ++i)
		engine->table[1 + i] =
			cpu_to_le32(lower_32_bits(engine->firmware_dma + i * PAGE_SIZE) | 1);

	static_assert(CDV_H264_COMMAND_WORDS * 4 <= PAGE_SIZE);
	static_assert(sizeof(cdv_h264_vlc) <= PAGE_SIZE);

	static_assert(CDV_H264_ARENA_VA + CDV_H264_ARENA_SIZE <= CDV_RENDEC_VA);
	engine->arena =
		kcalloc(CDV_H264_ARENA_SIZE / PAGE_SIZE, sizeof(*engine->arena), GFP_KERNEL);
	if (!engine->arena) {
		ret = -ENOMEM;
		goto free_directory;
	}
	{
		u32 slot, slots = CDV_H264_SURFACES;

		for (slot = 0; slot < slots; slot++) {
			struct cdv_surface_storage *storage;

			if (job && job->bound_output && slot == job->pictures[0].output) {
				storage = job->bound_output;
				kref_get(&storage->refs);
			} else {
				storage = cdv_storage_alloc(dev);
			}
			if (IS_ERR(storage)) {
				ret = PTR_ERR(storage);
				goto free_directory;
			}
			engine->surfaces[slot] = storage;
		}
	}
	for (i = 0; i < CDV_H264_ARENA_SIZE / PAGE_SIZE; ++i) {
		struct cdv_dma_page *page = &engine->arena[i];

		struct cdv_dma_page *owned = cdv_arena_storage_page(engine, i);

		if (owned)
			*page = *owned;
		else
			cdv_surface_page_alloc(dev, page, false);
		if (!page->cpu) {
			ret = -ENOMEM;
			goto free_directory;
		}
		engine->arena_allocated++;
		if (upper_32_bits(page->dma)) {
			ret = -ERANGE;
			goto free_directory;
		}
		memset(page->cpu, 0xa5, PAGE_SIZE);
		if (CDV_CODEC_AUX_BYTES &&
		    i * PAGE_SIZE >= CDV_H264_OUTPUT_OFFSET + CDV_SURFACE_SIZE)
			memset(page->cpu, 0, PAGE_SIZE);
		engine->table[16 + i] = cpu_to_le32(lower_32_bits(page->dma) | 1);
	}
	memcpy(engine->arena[0].cpu, cdv_h264_vlc, sizeof(cdv_h264_vlc));

	for (i = 0; i < CDV_H264_COMMAND_WORDS; ++i)
		engine->command[i] = cpu_to_le32(cdv_h264_commands[i]);
	/* Public DMA_sLinkedList layout, render-buffer VLD transfer.
	 * Observed DMA state shows this firmware consumes a descriptor
	 * at LLDMA_ADDRESS, rather than the flat command bytes there.
	 */
	static_assert(CDV_H264_COMMAND_WORDS * 4 < 0xc00);
	engine->command[0xc00 / 4 + 0] = 0; /* No swap, host->device, 32-bit. */
	engine->command[0xc00 / 4 + 1] = cpu_to_le32(0x80040000 | CDV_H264_COMMAND_WORDS);
	engine->command[0xc00 / 4 + 2] = cpu_to_le32(MTX_SYSC_CDMAT);
	engine->command[0xc00 / 4 + 3] = cpu_to_le32(1U << 26); /* Burst=1. */
	engine->command[0xc00 / 4 + 4] = 0;
	engine->command[0xc00 / 4 + 5] = 0;
	engine->command[0xc00 / 4 + 6] = cpu_to_le32(CDV_COMMAND_VA);
	engine->command[0xc00 / 4 + 7] = 0; /* End of list. */
	if (CDV_H264_LINKED_DMA) {
		/* Classic synchronous VLC and asynchronous bitstream DMA,
		 * using Intel's DMA_DETAIL_LOOKUP and DMA_LL_SET_WDn map.
		 */
		u32 descriptors[][8] = {
			{ 0x10000000, 0x80010000 | (sizeof(cdv_h264_vlc) / 2), 0x3000, 2U << 26, 0,
			  0, CDV_H264_ARENA_VA, 0 },
			{ 0x20000000, 0x80040000 | 0, 0x0820, 4U << 26, 0, 0,
			  CDV_H264_ARENA_VA + PAGE_SIZE, 0 },
		};
		u32 j, k;

		for (j = 0; j < ARRAY_SIZE(descriptors); ++j)
			for (k = 0; k < 8; ++k)
				engine->command[(0xc40 / 4) + j * 8 + k] =
					cpu_to_le32(descriptors[j][k]);
	}
	for (i = 0; i < ARRAY_SIZE(cdv_h264_preload_dma); ++i)
		engine->command[0xc80 / 4 + i] = cpu_to_le32(cdv_h264_preload_dma[i]);

	if (job && (job->version == 2 || job->version >= 4 ||
		    (keep && (CDV_CODEC_MODE == 3 || CDV_H264_PREDICTED)))) {
		static_assert(CDV_REFERENCES_VA +
				      CDV_REFERENCE_TABLE_BYTES / PAGE_SIZE * CDV_MMU_SPAN <=
			      CDV_BITSTREAM_VA);
		engine->reference_table = dma_alloc_wc(dev->dev, CDV_REFERENCE_TABLE_BYTES,
						       &engine->reference_table_dma,
						       GFP_KERNEL | GFP_DMA32);
		engine->reference_pages = kcalloc(CDV_REFERENCES_SIZE / PAGE_SIZE,
						  sizeof(*engine->reference_pages), GFP_KERNEL);
		if (!engine->reference_table || !engine->reference_pages) {
			ret = -ENOMEM;
			goto free_directory;
		}
		if (upper_32_bits(engine->reference_table_dma + CDV_REFERENCE_TABLE_BYTES - 1)) {
			ret = -ERANGE;
			goto free_directory;
		}
		memset(engine->reference_table, 0, CDV_REFERENCE_TABLE_BYTES);
		{
			for (i = 1; i < CDV_H264_SURFACES; i++) {
				if (engine->surfaces[i])
					continue;
				struct cdv_surface_storage *storage = cdv_storage_alloc(dev);

				if (IS_ERR(storage)) {
					ret = PTR_ERR(storage);
					goto free_directory;
				}
				engine->surfaces[i] = storage;
			}
		}
		for (i = 0; i < CDV_REFERENCES_SIZE / PAGE_SIZE; ++i) {
			struct cdv_dma_page *page = &engine->reference_pages[i];

			*page = engine->surfaces[1 + i / (CDV_SURFACE_SIZE / PAGE_SIZE)]
					->pages[i % (CDV_SURFACE_SIZE / PAGE_SIZE)];
			if (!page->cpu) {
				ret = -ENOMEM;
				goto free_directory;
			}
			engine->references_allocated++;
			if (upper_32_bits(page->dma)) {
				ret = -ERANGE;
				goto free_directory;
			}
			memset(page->cpu, 0xa5, PAGE_SIZE);
			drm_clflush_virt_range(page->cpu, PAGE_SIZE);
			engine->reference_table[i] = cpu_to_le32(lower_32_bits(page->dma) | 1);
		}
		for (i = 0; i < CDV_REFERENCE_TABLE_BYTES / PAGE_SIZE; i++) {
			dma_addr_t address = engine->reference_table_dma + i * PAGE_SIZE;

			engine->directory[(CDV_REFERENCES_VA >> 22) + i] =
				cpu_to_le32(lower_32_bits(address) | 1);
		}
	}
	{
		u32 pages = DIV_ROUND_UP(CEDAR_H264_MAX_SLICE_BYTES, PAGE_SIZE);

		static_assert(CEDAR_H264_DMA_EXTRA_OFFSET + (CEDAR_H264_DMA_DESCRIPTORS - 1) * 32 <=
			      PAGE_SIZE);
		static_assert(0xc80 + sizeof(cdv_h264_preload_dma) <= CEDAR_H264_DMA_EXTRA_OFFSET);
		static_assert(CEDAR_H264_MAX_SLICE_BYTES <= CDV_MMU_SPAN);
		engine->bitstream_table = dma_alloc_wc(dev->dev, PAGE_SIZE,
						       &engine->bitstream_table_dma,
						       GFP_KERNEL | GFP_DMA32);
		engine->bitstream = kcalloc(pages, sizeof(*engine->bitstream), GFP_KERNEL);
		if (!engine->bitstream_table || !engine->bitstream) {
			ret = -ENOMEM;
			goto free_directory;
		}
		if (upper_32_bits(engine->bitstream_table_dma + PAGE_SIZE - 1)) {
			ret = -ERANGE;
			goto free_directory;
		}
		memset(engine->bitstream_table, 0, PAGE_SIZE);
		for (i = 0; i < pages; i++) {
			struct cdv_dma_page *page = &engine->bitstream[i];

			cdv_surface_page_alloc(dev, page, false);
			if (!page->cpu) {
				ret = -ENOMEM;
				goto free_directory;
			}
			engine->bitstream_allocated++;
			atomic_inc(&bitstream_pages);
			if (upper_32_bits(page->dma)) {
				ret = -ERANGE;
				goto free_directory;
			}
			memset(page->cpu, 0, PAGE_SIZE);
			drm_clflush_virt_range(page->cpu, PAGE_SIZE);
			engine->bitstream_table[i] = cpu_to_le32(lower_32_bits(page->dma) | 1);
		}
		engine->directory[CDV_BITSTREAM_VA >> 22] =
			cpu_to_le32(lower_32_bits(engine->bitstream_table_dma) | 1);
		drm_clflush_virt_range(engine->bitstream_table, PAGE_SIZE);
	}
	ret = cdv_engine_start(dev, regs, engine, true);
	if (!ret)
		ret = cdv_engine_submit(dev, regs, engine, job);
	if (!ret && keep) {
		*keep = engine;
		return 0;
	}

free_directory:
	cleanup = cdv_engine_destroy(dev, regs, engine);
	return cleanup ? cleanup : ret;
}

static int cdv_restart_after_sleep(struct drm_device *dev, void __iomem *regs,
				   struct cdv_engine_context *engine)
{
	u32 i;
	int ret;

	writel(0x7f, regs + CDV_MSVDX_CLOCK);
	ret = cdv_reset_decoder(regs);
	if (ret)
		goto done;
	writel(0x7f, regs + CDV_MSVDX_CLOCK);
	writel(1, regs + MTX_SOFT_RESET);
	/* RENDEC is scratch. Only its queues are reset; NV12 and motion
	 * reference storage remain unchanged across the sleep checkpoint.
	 */
	for (i = 0; i < engine->rendec_allocated; i++) {
		memset(engine->rendec_pages[i].cpu, 0, PAGE_SIZE);
		drm_clflush_virt_range(engine->rendec_pages[i].cpu, PAGE_SIZE);
	}
	engine->next_sequence = 1;
	/* An established DPB must never satisfy fresh-intra abort/retry. */
	engine->command_completed = true;
	ret = cdv_engine_start(dev, regs, engine, false);
	if (!ret) {
		engine->needs_restart = false;
		resume_restarts++;
	}
done:
	if (ret)
		resume_failures++;
	dev_info(dev->dev, "cedar-msvdx: stage=resume_context result=%d\n", ret);
	return ret;
}

static int cdv_load_firmware(struct drm_device *dev, void __iomem *regs, u32 rev,
			     struct cdv_job *job, struct cdv_engine_context **keep)
{
	const struct firmware *fw;
	struct cdv_firmware_image image;
	u32 saved_clock, bank_size;
	int ret;

	/* Restrict writes to the exact core physically identified on this N2800. */
	if ((rev & 0xffffff) != 0x020103)
		return -ENODEV;
	ret = request_firmware(&fw, firmware_name, dev->dev);
	if (ret)
		return ret;
	ret = cdv_parse_firmware(fw, &image);
	if (ret)
		goto release;
	if (readl(regs + MTX_ENABLE) & 1) {
		ret = -EBUSY;
		goto release;
	}
	saved_clock = readl(regs + CDV_MSVDX_CLOCK);
	writel(0x7f, regs + CDV_MSVDX_CLOCK);

	ret = cdv_reset_decoder(regs);
	if (ret) {
		writel(saved_clock, regs + CDV_MSVDX_CLOCK);
		goto release;
	}
	/* Decoder reset also resets the local clock-enable register. */
	writel(0x7f, regs + CDV_MSVDX_CLOCK);

	writel(1, regs + MTX_SOFT_RESET);
	bank_size = 1U << (((readl(regs + CDV_MSVDX_RAM_BANK) >> 16) & 0xf) + 2);
	dev_info(dev->dev,
		 "cedar-msvdx: firmware=%s version=%x text_words=%u data_words=%u data_offset=%x bank_size=%u\n",
		 firmware_name, image.version, image.text_words, image.data_words,
		 image.data_offset, bank_size);
	ret = cdv_ram_transfer(regs, bank_size, 0x10, 0, image.text_words, image.text, false);
	if (!ret)
		ret = cdv_ram_transfer(regs, bank_size, 0x18, image.data_offset, image.data_words,
				       image.data, false);
	if (!ret)
		ret = cdv_ram_transfer(regs, bank_size, 0x10, 0, image.text_words, image.text,
				       true);
	if (!ret)
		ret = cdv_ram_transfer(regs, bank_size, 0x18, image.data_offset, image.data_words,
				       image.data, true);
	dev_info(dev->dev,
		 "cedar-msvdx: stage=firmware_ram result=%d verified_words=%u mtx_started=0\n", ret,
		 ret ? 0 : image.text_words + image.data_words);
	if (!ret)
		ret = cdv_boot_firmware(dev, regs, &image, job, keep);
	if (!ret && keep && *keep) {
		(*keep)->saved_clock = saved_clock;
	} else {
		/* Stop MTX when no live context retains the engine. */
		writel(1, regs + MTX_SOFT_RESET);
		if (ret != -EUCLEAN)
			writel(saved_clock, regs + CDV_MSVDX_CLOCK);
	}
release:
	release_firmware(fw);
	return ret;
}

/* Caller holds cdv_msvdx_lock; parent removal invokes this before BAR teardown. */
static int cdv_session_stop(struct cdv_session *session)
{
	u32 saved_clock;
	int ret;

	cdv_reference_reset(&session->references);
	session->config_valid = false;
	if (!session->engine)
		return 0;
	saved_clock = session->engine->saved_clock;
	ret = cdv_engine_destroy(session->dev, session->regs, session->engine);
	if (ret == -EUCLEAN)
		cdv_msvdx_quarantined = true;
	else
		writel(saved_clock, session->regs + CDV_MSVDX_CLOCK);
	dev_info(session->dev->dev, "cedar-msvdx: stage=session_close result=%d\n", ret);
	session->engine = NULL;
	if (cdv_active_session == session)
		cdv_active_session = NULL;
	if (ret == -EUCLEAN) {
		engine_power_retained = true;
	} else {
		iounmap(session->regs);
		pm_runtime_put(session->dev->dev);
	}
	session->regs = NULL;
	session->dev = NULL;
	session_active = 0;
	return ret;
}

static void cdv_job_free(struct cdv_job *job)
{
	u32 i;

	if (!job)
		return;
	if (job->children) {
		for (i = 0; i < job->slice_count; i++)
			cdv_job_free(job->children[i]);
		kfree(job->children);
	}
	if (job->owns_packet)
		kvfree(job->packet);
	if (job->bound_output)
		kref_put(&job->bound_output->refs, cdv_storage_destroy);
	kvfree(job->output);
	kfree(job);
}

static int cdv_msvdx_open(struct inode *inode, struct file *file)
{
	struct cdv_session *session;
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	session = kzalloc_obj(*session);
	if (!session)
		return -ENOMEM;
	ret = nonseekable_open(inode, file);
	if (ret) {
		kfree(session);
		return ret;
	}
	cdv_reference_reset(&session->references);
	file->private_data = session;
	return 0;
}

static int cdv_msvdx_release(struct inode *inode, struct file *file)
{
	struct cdv_session *session = file->private_data;
	int ret;
	u32 i;

	mutex_lock(&cdv_msvdx_lock);
	ret = cdv_session_stop(session);
	cdv_job_free(session->job);
	for (i = 0; i < CEDAR_SURFACE_HANDLES; i++)
		if (session->handles[i])
			kref_put(&session->handles[i]->refs, cdv_storage_destroy);
	kfree(session);
	mutex_unlock(&cdv_msvdx_lock);
	return ret;
}

static int cdv_parse_dpb_picture(struct cdv_job *job, struct cdv_reference_state *state)
{
	const struct cdv_picture *p = &job->pictures[0];
	const struct cedar_h264_dpb *r = &job->dpb;
	u32 poc = job->h264_pocs[0][0], list, i, j;
	bool wide = job->version == 14, explicit = job->version >= 13;
	u32 max_refs = wide ? job->full.max_refs : r->max_refs;
	u32 mask = wide ? job->full.retained_mask : r->reserved[0];
	u32 reference = wide ? job->full.reference : r->reserved[1];
	u32 l0_count = wide ? job->full.l0_count : r->l0_count;
	u32 l1_count = wide ? job->full.l1_count : r->l1_count;
	const u32 *l0 = wide ? job->full.l0_slots : r->l0_slots;
	const u32 *l1 = wide ? job->full.l1_slots : r->l1_slots;
	const u32 *poc0 = wide ? job->full.l0_pocs : r->l0_pocs;
	const u32 *poc1 = wide ? job->full.l1_pocs : r->l1_pocs;

	if (p->output >= CEDAR_H264_SURFACES)
		return -EINVAL;
	if (p->type == 5) {
		if (poc || job->h264_pocs[0][1] || job->h264_pocs[0][2] || p->forward != U32_MAX ||
		    p->backward != U32_MAX || job->h264_cabac_init[0] || job->h264_direct[0])
			return -EINVAL;
		cdv_reference_reset(state);
		state->dpb_limit = max_refs;
		state->stable_refs = true;
		state->p_refs = state->stable_refs;
	} else {
		if (wide && p->type == 6 && !state->dpb_limit) {
			if (mask || state->dpb_count)
				return -EINVAL;
			state->dpb_limit = max_refs;
			state->stable_refs = true;
			state->p_refs = state->stable_refs;
		}
		if (explicit) {
			u32 known = 0, retained = 0;

			for (i = 0; i < state->dpb_count; i++) {
				known |= BIT(state->dpb_slots[i]);
				if (!(mask & BIT(state->dpb_slots[i])))
					continue;
				state->dpb_slots[retained] = state->dpb_slots[i];
				state->dpb_pocs[retained++] = state->dpb_pocs[i];
			}
			if (mask & ~known)
				return -EINVAL;
			state->dpb_count = retained;
		}
		if ((p->type != 6 && !state->dpb_count) || state->dpb_limit != max_refs ||
		    p->forward != l0[0] || job->h264_pocs[0][1] != poc0[0])
			return -EINVAL;
		for (i = 0; i < state->dpb_count; i++) {
			if (p->output == state->dpb_slots[i])
				return -EINVAL;
			if (wide && abs((s64)state->dpb_pocs[i] - poc) > 65535)
				return -EINVAL;
			job->dpb_pocs[state->dpb_slots[i]] = state->dpb_pocs[i];
		}
		for (list = 0; list < 2; list++) {
			u32 count = list ? l1_count : l0_count;
			const u32 *slots = list ? l1 : l0;
			const u32 *pocs = list ? poc1 : poc0;

			for (i = 0; i < count; i++) {
				for (j = 0; j < state->dpb_count; j++)
					if (slots[i] == state->dpb_slots[j] &&
					    pocs[i] == state->dpb_pocs[j])
						break;
				if (j == state->dpb_count)
					return -EINVAL;
			}
		}
		if (p->type == 1 || p->type == 6) {
			if ((!explicit && poc <= state->last_poc) || p->backward != U32_MAX ||
			    job->h264_pocs[0][2] || job->h264_direct[0])
				return -EINVAL;
		} else if ((!explicit && (state->dpb_count < 2 || poc <= state->previous_poc ||
					  poc >= state->last_poc)) ||
			   p->backward != l1[0] || job->h264_pocs[0][2] != poc1[0]) {
			return -EINVAL;
		}
	}
	if (explicit ? reference : p->type != 2) {
		if (!explicit && state->dpb_count == state->dpb_limit) {
			state->dpb_count--;
			memmove(state->dpb_slots, state->dpb_slots + 1,
				state->dpb_count * sizeof(u32));
			memmove(state->dpb_pocs, state->dpb_pocs + 1,
				state->dpb_count * sizeof(u32));
		}
		if (state->dpb_count >= ARRAY_SIZE(state->dpb_slots))
			return -EINVAL;
		state->dpb_slots[state->dpb_count] = p->output;
		state->dpb_pocs[state->dpb_count++] = poc;
		state->previous = state->last;
		state->previous_poc = state->last_poc;
		state->last = p->output;
		state->last_poc = poc;
	}
	return 0;
}

static int cdv_parse_stable_refs(struct cdv_job *job, const struct cdv_reference_state *state,
				 const struct cdv_picture *p)
{
	const struct cedar_h264_refs *r = &job->refs;
	u32 list, i;

	if ((p->type != 1 && p->type != 2) || state->last > 2 ||
	    (state->p_refs && !state->stable_refs) || p->output == state->last ||
	    p->output == state->previous || p->forward != r->l0_slots[0] ||
	    job->h264_pocs[0][1] != r->l0_pocs[0] ||
	    (p->type == 2 && (!state->stable_refs || p->backward != r->l1_slots[0] ||
			      job->h264_pocs[0][2] != r->l1_pocs[0])))
		return -EINVAL;
	for (list = 0; list < 2; list++) {
		u32 count = list ? r->l1_count : r->l0_count;
		const u32 *slots = list ? r->l1_slots : r->l0_slots;
		const u32 *pocs = list ? r->l1_pocs : r->l0_pocs;

		for (i = 0; i < count; i++)
			if (!((slots[i] == state->last && pocs[i] == state->last_poc) ||
			      (state->previous <= 2 && slots[i] == state->previous &&
			       pocs[i] == state->previous_poc)))
				return -EINVAL;
	}
	job->dpb_pocs[state->last] = state->last_poc;
	if (state->previous <= 2)
		job->dpb_pocs[state->previous] = state->previous_poc;
	return 0;
}

static int cdv_parse_bidirectional_pictures(struct cdv_job *job,
					    const struct cdv_reference_state *previous_state)
{
	struct cdv_reference_state state;
	u32 frame, displays = 0;

	if (job->version >= 6)
		state = *previous_state;
	else
		cdv_reference_reset(&state);

	for (frame = 0; frame < job->frames; ++frame) {
		const u8 *d = job->packet + 32 + frame * 64;
		struct cdv_picture *p = &job->pictures[frame];
		u32 index, poc, fpoc, bpoc;

		job->h264_offsets[frame] = get_unaligned_le32(d);
		job->h264_qps[frame] = get_unaligned_le32(d + 4);
		p->display = get_unaligned_le32(d + 8);
		p->type = get_unaligned_le32(d + 12);
		p->output = get_unaligned_le32(d + 16);
		p->forward = get_unaligned_le32(d + 20);
		p->backward = get_unaligned_le32(d + 24);
		job->h264_cabac_init[frame] = get_unaligned_le32(d + 28);
		for (index = 0; index < 3; ++index)
			job->h264_pocs[frame][index] = get_unaligned_le32(d + 32 + index * 4);
		poc = job->h264_pocs[frame][0];
		fpoc = job->h264_pocs[frame][1];
		bpoc = job->h264_pocs[frame][2];
		job->h264_direct[frame] = get_unaligned_le32(d + 44);
		if (job->h264_offsets[frame] < 8 || job->h264_offsets[frame] > 512 ||
		    job->h264_qps[frame] > 51 || job->h264_cabac_init[frame] > 2 ||
		    job->h264_direct[frame] > 1 || p->display >= job->frames ||
		    (displays & BIT(p->display)) ||
		    (job->version == 5 ?
			     poc != p->display * 2 :
			     (poc > (job->version == 14 ? 0x7ffffffeU : 65534) || (poc & 1))) ||
		    p->output > (job->version >= 12 ? 4 : 2) ||
		    (p->type != 1 && p->type != 2 && p->type != 5 &&
		     !(job->version == 14 && p->type == 6)))
			return -EINVAL;
		for (index = job->version >= 6 ? 52 : 48; index < 64; index += 4)
			if (get_unaligned_le32(d + index))
				return -EINVAL;
		if (job->version >= 12) {
			if (cdv_parse_dpb_picture(job, &state))
				return -EINVAL;
			displays |= BIT(p->display);
			continue;
		}
		if (state.dpb_limit && p->type != 5)
			return -EINVAL;
		if ((job->version == 9 && p->type != 1) ||
		    (state.p_refs && p->type != 5 && job->version != 9 && job->version != 11) ||
		    (state.stable_refs && p->type != 5 && job->version != 11))
			return -EINVAL;
		if (job->version == 11 && cdv_parse_stable_refs(job, &state, p))
			return -EINVAL;
		if (p->type == 5) {
			if (frame || poc || fpoc || bpoc || p->forward != U32_MAX ||
			    p->backward != U32_MAX || job->h264_cabac_init[frame] ||
			    job->h264_direct[frame])
				return -EINVAL;
			cdv_reference_reset(&state);
			state.last = p->output;
			state.last_poc = 0;
		} else if (p->type == 1) {
			if (state.last > 2 || poc <= state.last_poc || p->output == state.last ||
			    p->backward != U32_MAX || bpoc || job->h264_direct[frame])
				return -EINVAL;
			if (job->version == 11) {
				state.stable_refs = true;
				state.p_refs = state.stable_refs;
			} else if (job->version == 9) {
				const struct cedar_h264_p_refs *refs = &job->p_refs;

				if (p->output == state.previous || p->forward != refs->slots[0] ||
				    fpoc != refs->pocs[0])
					return -EINVAL;
				for (index = 0; index < refs->count; index++)
					if (!((refs->slots[index] == state.last &&
					       refs->pocs[index] == state.last_poc) ||
					      (state.previous <= 2 &&
					       refs->slots[index] == state.previous &&
					       refs->pocs[index] == state.previous_poc)))
						return -EINVAL;
				state.p_refs = true;
			} else if (p->forward != state.last || fpoc != state.last_poc) {
				return -EINVAL;
			}
			state.previous = state.last;
			state.previous_poc = state.last_poc;
			state.last = p->output;
			state.last_poc = poc;
		} else if (state.previous > 2 || state.last > 2 ||
			   (job->version != 11 &&
			    (p->forward != state.previous || p->backward != state.last ||
			     fpoc != state.previous_poc || bpoc != state.last_poc)) ||
			   p->output == state.previous || p->output == state.last ||
			   poc <= state.previous_poc || poc >= state.last_poc) {
			return -EINVAL;
		}
		displays |= BIT(p->display);
	}
	job->next_references = state;
	return 0;
}

static int cdv_parse_pictures(struct cdv_job *job, const struct cdv_reference_state *previous_state)
{
	u32 frame, anchors = 0, displays = 0;

	if (job->version >= 5)
		return cdv_parse_bidirectional_pictures(job, previous_state);

	for (frame = 0; frame < job->frames; ++frame) {
		struct cdv_picture *picture = &job->pictures[frame];
		const u8 *descriptor;
		u32 nibble;

		if (job->version == 1) {
			picture->type = 1;
			picture->f_code = 0xffff;
			picture->backward = U32_MAX;
			picture->forward = picture->backward;
			picture->display = frame;
			continue;
		}
		descriptor = job->packet + 32 + frame * 32;
		if (job->version == 3 || job->version == 4) {
			u32 reserved;

			picture->type = 4;
			picture->display = get_unaligned_le32(descriptor + 8);
			picture->backward = U32_MAX;
			picture->forward = picture->backward;
			job->h264_offsets[frame] = get_unaligned_le32(descriptor);
			job->h264_qps[frame] = get_unaligned_le32(descriptor + 4);
			if (picture->display >= job->frames || (displays & BIT(picture->display)) ||
			    job->h264_offsets[frame] < 8 || job->h264_offsets[frame] > 512 ||
			    job->h264_qps[frame] > 51)
				return -EINVAL;
			if (job->version == 4) {
				picture->type = get_unaligned_le32(descriptor + 12);
				picture->output = get_unaligned_le32(descriptor + 16);
				picture->forward = get_unaligned_le32(descriptor + 20);
				job->h264_cabac_init[frame] = get_unaligned_le32(descriptor + 24);
				if ((picture->type != 1 && picture->type != 5) ||
				    picture->output > 1 || picture->display != frame ||
				    job->h264_cabac_init[frame] > 2)
					return -EINVAL;
				if (picture->type == 5) {
					if (picture->forward != U32_MAX ||
					    job->h264_cabac_init[frame])
						return -EINVAL;
				} else if (picture->forward > 1 ||
					   picture->forward == picture->output ||
					   anchors != BIT(picture->forward)) {
					return -EINVAL;
				}
				anchors = BIT(picture->output);
			}
			for (reserved = job->version == 4 ? 28 : 12; reserved < 32; reserved += 4)
				if (get_unaligned_le32(descriptor + reserved))
					return -EINVAL;
			displays |= BIT(picture->display);
			continue;
		}
		picture->type = get_unaligned_le32(descriptor);
		picture->f_code = get_unaligned_le32(descriptor + 4);
		picture->output = get_unaligned_le32(descriptor + 8);
		picture->forward = get_unaligned_le32(descriptor + 12);
		picture->backward = get_unaligned_le32(descriptor + 16);
		picture->display = get_unaligned_le32(descriptor + 20);
		if (!picture->type || picture->type > 3 || picture->f_code > 0xffff ||
		    picture->output > 2 || picture->display >= job->frames ||
		    (displays & BIT(picture->display)) || get_unaligned_le32(descriptor + 24) ||
		    get_unaligned_le32(descriptor + 28))
			return -EINVAL;
		for (nibble = 0; nibble < 4; ++nibble)
			if (!((picture->f_code >> (nibble * 4)) & 15))
				return -EINVAL;
		if (picture->type == 1) {
			if (picture->forward != U32_MAX || picture->backward != U32_MAX)
				return -EINVAL;
			anchors = 0;
		} else {
			if (picture->forward > 2 || picture->forward == picture->output ||
			    !(anchors & BIT(picture->forward)))
				return -EINVAL;
			if (picture->type == 2 && picture->backward != U32_MAX)
				return -EINVAL;
			if (picture->type == 3 &&
			    (picture->backward > 2 || picture->backward == picture->output ||
			     picture->backward == picture->forward ||
			     !(anchors & BIT(picture->backward))))
				return -EINVAL;
		}
		displays |= BIT(picture->display);
		anchors &= ~BIT(picture->output);
		if (picture->type != 3)
			anchors |= BIT(picture->output);
	}
	return 0;
}

static struct cdv_job *cdv_job_parse_packet(u8 *buffer, size_t count,
					    const struct cdv_session *session, bool owns_packet,
					    bool allocate_output)
{
	struct cdv_job *job;
	u32 slices, slots, offset, lengths_offset, i;
	size_t limit = 32 + 256 + CDV_BATCH_FRAMES * 64 +
		       CDV_BATCH_FRAMES * CDV_H264_SLICE_COUNT *
			       (4 + CDV_H264_BITSTREAM_PAGES * PAGE_SIZE);
	int ret = -EINVAL;

	job = kzalloc_obj(*job);
	if (!job) {
		if (owns_packet)
			kvfree(buffer);
		return ERR_PTR(-ENOMEM);
	}
	job->packet = buffer;
	job->owns_packet = owns_packet;
	if (count < 32) {
		ret = -EMSGSIZE;
		goto fail;
	}
	job->version = get_unaligned_le32(job->packet + 4);
	if (job->version == 10) {
		u32 position = 32, previous_mb = 0;

		if (!owns_packet || !CDV_H264_MULTI_SLICE || CDV_CODEC_MODE != 1 ||
		    count > CEDAR_H264_MULTI_LIMIT || memcmp(buffer, "CDV2", 4) ||
		    get_unaligned_le32(buffer + 28) != count - 32 ||
		    get_unaligned_le32(buffer + 20) != 1)
			goto fail;
		job->slice_count = get_unaligned_le32(buffer + 16);
		job->flags = get_unaligned_le32(buffer + 24);
		if (job->slice_count < 2 || job->slice_count > CEDAR_H264_MAX_SLICES ||
		    (job->flags & ~3U) || job->flags == 3)
			goto fail;
		job->children = kcalloc(job->slice_count, sizeof(*job->children), GFP_KERNEL);
		if (!job->children) {
			ret = -ENOMEM;
			goto fail;
		}
		for (i = 0; i < job->slice_count; i++) {
			struct cdv_job *child;
			u32 first_mb, bytes, version;

			if (count - position < 8)
				goto fail;
			first_mb = get_unaligned_le32(buffer + position);
			bytes = get_unaligned_le32(buffer + position + 4);
			position += 8;
			if (bytes < 32 || bytes > count - position ||
			    (!i ? first_mb != 0 : first_mb <= previous_mb))
				goto fail;
			version = get_unaligned_le32(buffer + position + 4);
			if ((version < 7 || (version > 9 && version != 11 && version != 12 &&
					     version != 13 && version != 14)) ||
			    get_unaligned_le32(buffer + position + 24))
				goto fail;
			child = cdv_job_parse_packet(buffer + position, bytes, session, false,
						     false);
			if (IS_ERR(child)) {
				ret = PTR_ERR(child);
				goto fail;
			}
			job->children[i] = child;
			child->first_mb = first_mb;
			child->slice_index = i;
			if (child->frames != 1 || first_mb >= child->width * child->height / 256 ||
			    child->width != get_unaligned_le32(buffer + 8) ||
			    child->height != get_unaligned_le32(buffer + 12))
				goto fail;
			if (!i) {
				job->width = child->width;
				job->height = child->height;
				job->frame_bytes = child->frame_bytes;
				job->frames = 1;
				job->config = child->config;
				job->pictures[0] = child->pictures[0];
				job->next_references = child->next_references;
			} else {
				const struct cdv_reference_state *a = &job->next_references,
								 *b = &child->next_references;

				if (version != job->children[0]->version ||
				    !cedar_h264_sequence_equal(&job->config, &child->config) ||
				    child->pictures[0].type != job->pictures[0].type ||
				    child->pictures[0].output != job->pictures[0].output ||
				    child->h264_pocs[0][0] != job->children[0]->h264_pocs[0][0] ||
				    a->last != b->last || a->previous != b->previous ||
				    a->last_poc != b->last_poc ||
				    a->previous_poc != b->previous_poc || a->p_refs != b->p_refs ||
				    a->stable_refs != b->stable_refs ||
				    a->dpb_count != b->dpb_count || a->dpb_limit != b->dpb_limit ||
				    memcmp(a->dpb_slots, b->dpb_slots, sizeof(a->dpb_slots)) ||
				    memcmp(a->dpb_pocs, b->dpb_pocs, sizeof(a->dpb_pocs)))
					goto fail;
			}
			position += bytes;
			previous_mb = first_mb;
		}
		if (position != count)
			goto fail;
		if (allocate_output) {
			job->output = kvmalloc(job->frame_bytes, GFP_KERNEL | (__GFP_ZERO));
			if (!job->output) {
				ret = -ENOMEM;
				goto fail;
			}
		}
		return job;
	}
	if (count >
	    max_t(size_t, limit, CEDAR_H264_FULL_LENGTH_OFFSET + 4 + CEDAR_H264_MAX_SLICE_BYTES)) {
		ret = -EMSGSIZE;
		goto fail;
	}
	if (CDV_CODEC_MODE == 1) {
		bool bidirectional =
			CDV_H264_BIDIRECTIONAL &&
			(job->version == 5 ||
			 ((job->version == 6 ||
			   (CDV_H264_RUNTIME_TEMPLATE &&
			    (job->version == 7 || (CDV_H264_EXPLICIT_P && job->version == 8) ||
			     (CDV_H264_P_REF2 && job->version == 9) ||
			     (CDV_H264_STABLE_REFS && job->version == 11) ||
			     (CDV_H264_DPB4 && (job->version == 12 || job->version == 13 ||
						job->version == 14)))))));

		if (job->version != 3 && !(CDV_H264_PREDICTED && job->version == 4) &&
		    !bidirectional)
			goto fail;
	} else if (job->version != 1 && job->version != 2) {
		goto fail;
	}
	if (memcmp(job->packet, "CDV2", 4) || get_unaligned_le32(job->packet + 28) != count - 32)
		goto fail;
	job->width = get_unaligned_le32(job->packet + 8);
	job->height = get_unaligned_le32(job->packet + 12);
	if (job->version >= 7) {
		u32 words[16];

		static_assert(sizeof(job->config) == sizeof(words));
		if (count < 164)
			goto fail;
		for (i = 0; i < ARRAY_SIZE(words); i++)
			words[i] = get_unaligned_le32(job->packet + 96 + i * 4);
		memcpy(&job->config, words, sizeof(words));
		if (!cedar_h264_config_valid(&job->config, CDV_H264_WIDTH, CDV_H264_HEIGHT) ||
		    job->width != job->config.width || job->height != job->config.height ||
		    (job->version == 7 && (job->config.config_flags & CEDAR_CONFIG_WEIGHT_P)))
			goto fail;
		/* Sequence/PPS changes require IDR; slice deblocking may vary.
		 * Compare only the first eight config words (through chroma QP).
		 */
		if (get_unaligned_le32(job->packet + 44) != 5 &&
		    !(job->version == 14 && get_unaligned_le32(job->packet + 44) == 6 &&
		      !session->config_valid) &&
		    (!session->config_valid ||
		     !cedar_h264_sequence_equal(&session->config, &job->config)))
			goto fail;
		if (job->version >= 8 && job->version < 12) {
			if (count < 228)
				goto fail;
			for (i = 0; i < ARRAY_SIZE(words); i++)
				words[i] = get_unaligned_le32(job->packet + 160 + i * 4);
			static_assert(sizeof(job->weights) == sizeof(words));
			memcpy(&job->weights, words, sizeof(words));
			if (!cedar_h264_weights_valid(&job->weights,
						      get_unaligned_le32(job->packet + 44) == 1 &&
							      (job->config.config_flags &
							       CEDAR_CONFIG_WEIGHT_P)))
				goto fail;
		}
		if (job->version == 14) {
			u32 j, type = get_unaligned_le32(job->packet + 44);

			static_assert(sizeof(job->full) == 544);
			if (count < CEDAR_H264_FULL_LENGTH_OFFSET + 4)
				goto fail;
			for (i = 0; i < sizeof(job->full) / 4; i++)
				((u32 *)&job->full)[i] =
					get_unaligned_le32(job->packet + 160 + i * 4);
			if (!cedar_h264_dpb32_valid(&job->full, type))
				goto fail;
			for (j = 0; j < 32; j++) {
				struct cedar_h264_weights *w = &job->full_weights[j];
				bool active = type == 1 && j < job->full.l0_count &&
					      (job->config.config_flags & CEDAR_CONFIG_WEIGHT_P);

				for (i = 0; i < 16; i++)
					((u32 *)w)[i] = get_unaligned_le32(job->packet + 704 +
									   j * 64 + i * 4);
				if (!cedar_h264_weights_valid(w, active) ||
				    (active &&
				     (w->luma_denom != job->full_weights[0].luma_denom ||
				      w->chroma_denom != job->full_weights[0].chroma_denom)))
					goto fail;
			}
		}
		if (job->version == 12 || job->version == 13) {
			u32 j, type = get_unaligned_le32(job->packet + 44);
			struct cedar_h264_weights *weights[] = { &job->weights, &job->weights1,
								 &job->weights_extra[0],
								 &job->weights_extra[1] };

			static_assert(sizeof(job->dpb) == 128);
			if (count < 548)
				goto fail;
			for (i = 0; i < sizeof(job->dpb) / 4; i++)
				((u32 *)&job->dpb)[i] =
					get_unaligned_le32(job->packet + 160 + i * 4);
			if (job->version == 13 ? !cedar_h264_explicit_dpb_valid(&job->dpb, type) :
						 !cedar_h264_dpb_valid(&job->dpb, type))
				goto fail;
			for (j = 0; j < ARRAY_SIZE(weights); j++) {
				for (i = 0; i < ARRAY_SIZE(words); i++)
					words[i] = get_unaligned_le32(job->packet + 288 + j * 64 +
								      i * 4);
				memcpy(weights[j], words, sizeof(words));
				if (!cedar_h264_weights_valid(weights[j],
							      type == 1 && j < job->dpb.l0_count &&
								      (job->config.config_flags &
								       CEDAR_CONFIG_WEIGHT_P)))
					goto fail;
				if (type == 1 && j < job->dpb.l0_count &&
				    (job->config.config_flags & CEDAR_CONFIG_WEIGHT_P) &&
				    (weights[j]->luma_denom != job->weights.luma_denom ||
				     weights[j]->chroma_denom != job->weights.chroma_denom))
					goto fail;
			}
		}
		if (job->version == 11) {
			if (count < 356)
				goto fail;
			static_assert(sizeof(job->refs) == sizeof(words));
			for (i = 0; i < ARRAY_SIZE(words); i++)
				words[i] = get_unaligned_le32(job->packet + 224 + i * 4);
			memcpy(&job->refs, words, sizeof(words));
			if (!cedar_h264_refs_valid(&job->refs,
						   get_unaligned_le32(job->packet + 44) == 2))
				goto fail;
			for (i = 0; i < ARRAY_SIZE(words); i++)
				words[i] = get_unaligned_le32(job->packet + 288 + i * 4);
			memcpy(&job->weights1, words, sizeof(words));
			if (!cedar_h264_weights_valid(&job->weights1,
						      job->refs.l0_count == 2 &&
							      get_unaligned_le32(job->packet +
										 44) == 1 &&
							      (job->config.config_flags &
							       CEDAR_CONFIG_WEIGHT_P)) ||
			    (job->refs.l0_count == 2 &&
			     (job->config.config_flags & CEDAR_CONFIG_WEIGHT_P) &&
			     (job->weights.luma_denom != job->weights1.luma_denom ||
			      job->weights.chroma_denom != job->weights1.chroma_denom)))
				goto fail;
		}
		if (job->version == 9) {
			if (count < 356)
				goto fail;
			static_assert(sizeof(job->p_refs) == sizeof(words));
			for (i = 0; i < ARRAY_SIZE(words); i++)
				words[i] = get_unaligned_le32(job->packet + 224 + i * 4);
			memcpy(&job->p_refs, words, sizeof(words));
			if (!cedar_h264_p_refs_valid(&job->p_refs))
				goto fail;
			for (i = 0; i < ARRAY_SIZE(words); i++)
				words[i] = get_unaligned_le32(job->packet + 288 + i * 4);
			memcpy(&job->weights1, words, sizeof(words));
			if (!cedar_h264_weights_valid(&job->weights1,
						      job->p_refs.count == 2 &&
							      (job->config.config_flags &
							       CEDAR_CONFIG_WEIGHT_P)))
				goto fail;
			if (job->p_refs.count == 2 &&
			    (job->config.config_flags & CEDAR_CONFIG_WEIGHT_P) &&
			    (job->weights.luma_denom != job->weights1.luma_denom ||
			     job->weights.chroma_denom != job->weights1.chroma_denom))
				goto fail;
		}
	} else if (job->width != CDV_H264_WIDTH || job->height != CDV_H264_HEIGHT ||
		   session->config_valid) {
		goto fail;
	}
	job->frame_bytes = job->width * job->height * 3 / 2;
	slices = get_unaligned_le32(job->packet + 16);
	job->frames = get_unaligned_le32(job->packet + 20);
	job->flags = get_unaligned_le32(job->packet + 24);
	if (slices != CDV_H264_SLICE_COUNT || !job->frames || job->frames > CDV_BATCH_FRAMES ||
	    (job->flags & ~1U) || (job->version >= 6 && job->frames != 1))
		goto fail;
	slots = slices * job->frames;
	lengths_offset = 32 + (job->version != 1 ? job->frames * (job->version >= 5 ? 64 : 32) : 0);
	if (job->version >= 7)
		lengths_offset += 64;
	if (job->version >= 8 && job->version < 12)
		lengths_offset += 64;
	if (job->version == 12 || job->version == 13)
		lengths_offset += 384;
	if (job->version == 14)
		lengths_offset = CEDAR_H264_FULL_LENGTH_OFFSET;
	if (job->version == 9 || job->version == 11)
		lengths_offset += 128;
	offset = lengths_offset + slots * 4;
	if (offset > count)
		goto fail;
	if (cdv_parse_pictures(job, &session->references))
		goto fail;
	for (i = 0; i < slots; ++i) {
		const u8 *slice;
		u32 bytes = get_unaligned_le32(job->packet + lengths_offset + i * 4);

		if (bytes < (job->version >= 3 ? 2 : 6) ||
		    bytes > (job->version == 14 ?
				     CEDAR_H264_MAX_SLICE_BYTES :
				     min_t(u32, 65535, CDV_H264_BITSTREAM_PAGES * PAGE_SIZE)) ||
		    bytes > count - offset)
			goto fail;
		slice = job->packet + offset;
		/* One complete slice per macroblock row, no extra data.
		 * Header and picture syntax are further checked by the packer.
		 */
		if (job->version >= 3) {
			u32 type = job->version == 3 ? 5 : job->pictures[i].type;
			u32 kind = type == 5 ? 5 : 1;

			if ((slice[0] & 0x9f) != kind ||
			    (job->version >= 13 ?
				     (!!(slice[0] & 0x60) != (job->version == 14 ?
								      job->full.reference :
								      job->dpb.reserved[1])) :
				     (type == 2 ? (slice[0] & 0x60) : !(slice[0] & 0x60))) ||
			    job->h264_offsets[i] >= bytes * 8)
				goto fail;
		} else {
			if (slice[0] || slice[1] || slice[2] != 1 || slice[3] != i % slices + 1 ||
			    (job->version == 1 ? (slice[4] & 0xfc) != 0x10 :
						 (!(slice[4] >> 3) || (slice[4] & 4))))
				goto fail;
		}
		job->offsets[i] = offset;
		job->lengths[i] = bytes;
		offset += bytes;
	}
	if (offset != count)
		goto fail;
	if (allocate_output) {
		/* available stays zero until snapshot has filled the exposed range. */
		job->output = kvmalloc(job->frames * job->frame_bytes, GFP_KERNEL | (__GFP_ZERO));
		if (!job->output) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	return job;
fail:
	cdv_job_free(job);
	return ERR_PTR(ret);
}

static struct cdv_job *cdv_job_parse(const char __user *buffer, size_t count,
				     const struct cdv_session *session)
{
	u8 *packet;
	struct cdv_job *job;
	size_t single_limit = 32 + 256 + CDV_BATCH_FRAMES * 64 +
			      CDV_BATCH_FRAMES * CDV_H264_SLICE_COUNT *
				      (4 + CDV_H264_BITSTREAM_PAGES * PAGE_SIZE);

	single_limit = max_t(size_t, single_limit,
			     CEDAR_H264_FULL_LENGTH_OFFSET + 4 + CEDAR_H264_MAX_SLICE_BYTES);

	if (count < 32 ||
	    count > max_t(size_t, single_limit, CDV_H264_MULTI_SLICE ? CEDAR_H264_MULTI_LIMIT : 0))
		return ERR_PTR(-EMSGSIZE);
	packet = vmemdup_user(buffer, count);
	if (IS_ERR(packet))
		return ERR_CAST(packet);
	job = cdv_job_parse_packet(packet, count, session, true, false);
	if (!IS_ERR(job) && (job->version < 7 || job->frames != 1 || job->flags ||
			     !cedar_h264_picture_geometry_valid(job->width, job->height) ||
			     job->frame_bytes != job->width * job->height * 3 / 2)) {
		cdv_job_free(job);
		return ERR_PTR(-EOPNOTSUPP);
	}
	return job;
}

static ssize_t cdv_msvdx_write(struct file *file, const char __user *buffer, size_t count,
			       loff_t *position)
{
	struct cdv_session *session = file->private_data;
	struct cdv_job *job;
	struct drm_device *dev;
	struct pci_dev *pdev;
	void __iomem *regs;
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	ret = mutex_lock_interruptible(&cdv_msvdx_lock);
	if (ret)
		return ret;
	dev = cdv_msvdx_dev;
	if (!dev || cdv_msvdx_quarantined) {
		ret = dev ? -EUCLEAN : -ENODEV;
		goto unlock;
	}
	if (cdv_active_session && cdv_active_session != session) {
		ret = -EBUSY;
		goto unlock;
	}

	job = cdv_job_parse(buffer, count, session);
	if (IS_ERR(job)) {
		ret = PTR_ERR(job);
		goto unlock;
	}
	if ((CDV_CODEC_MODE != 1 || !CDV_H264_RUNTIME_TEMPLATE || job->version < 7 ||
	     job->frames != 1 || job->flags || (job->children && job->children[0]->version != 14) ||
	     !cedar_h264_picture_geometry_valid(job->width, job->height))) {
		cdv_job_free(job);
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	if ((CDV_H264_SURFACES != 5 || !CDV_H264_COLOCATED_BYTES)) {
		cdv_job_free(job);
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	if ((job->width != 1280 || job->height != 720) && job->version != 14 &&
	    !(job->children && job->children[0]->version == 14)) {
		cdv_job_free(job);
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	if (session->pending_handle) {
		struct cdv_surface_storage *storage = session->handles[session->pending_handle - 1];
		u32 i;

		if (job->pictures[0].output != session->pending_slot) {
			cdv_job_free(job);
			ret = -EINVAL;
			goto unlock;
		}
		if (atomic_read(&storage->readers)) {
			cdv_job_free(job);
			ret = -EBUSY;
			goto unlock;
		}
		if (session->engine)
			for (i = 0; i < CDV_H264_SURFACES; i++)
				if (i != session->pending_slot &&
				    session->engine->surfaces[i] == storage) {
					cdv_job_free(job);
					ret = -EBUSY;
					goto unlock;
				}
		job->bound_output = storage;
		kref_get(&storage->refs);
	}
	/* Display ownership is independent of the H.264 reference set. Reject
	 * before replacing the last completed job or modifying hardware pages.
	 */
	if ((cdv_engine_readers(session->engine)) &&
	    (job->version < 7 || job->frames != 1 || job->flags ||
	     !cedar_h264_picture_geometry_valid(job->width, job->height) ||
	     !session->config_valid || !cedar_h264_sequence_equal(&job->config, &session->config) ||

	     (!job->bound_output && cdv_slot_readers(session->engine, job->pictures[0].output)))) {
		shared_rejected_writes++;
		cdv_job_free(job);
		ret = -EBUSY;
		goto unlock;
	}
	cdv_job_free(session->job);
	session->job = job;
	session->read_position = 0;
	*position = 0;
	if (session->engine) {
		ret = session->engine->needs_restart ?
			      cdv_restart_after_sleep(dev, session->regs, session->engine) :
			      0;
		if (!ret)
			ret = cdv_engine_submit(dev, session->regs, session->engine, job);
		if (ret) {
			int cleanup = cdv_session_stop(session);

			if (cleanup)
				ret = cleanup;
		}
		goto complete;
	}
	pdev = to_pci_dev(dev->dev);
	ret = pm_runtime_resume_and_get(dev->dev);
	if (ret < 0)
		goto unlock;
	regs = ioremap(pci_resource_start(pdev, 0) + CDV_MSVDX_OFFSET, CDV_MSVDX_SIZE);
	if (!regs) {
		ret = -ENOMEM;
	} else {
		unsigned int aborted_before = forced_resets, completed_before = completed_frames;

		ret = cdv_load_firmware(dev, regs, readl(regs + CDV_MSVDX_REV), job,
					&session->engine);
		/* One bounded restart of a fresh intra picture, only after the
		 * owned abort proved isolation and released the failed engine.
		 * No completed/visible picture or established DPB is replayed.
		 */
		if (ret == -EIO && !session->engine && !session->config_valid &&
		    forced_resets == aborted_before + 1 && completed_frames == completed_before &&
		    (job->version == 14 || (job->children && job->children[0]->version == 14)) &&
		    job->frames == 1 && !job->flags && !job->hardware_complete && !job->available &&
		    (job->pictures[0].type == 5 || job->pictures[0].type == 6)) {
			startup_retries++;
			dev_warn(dev->dev,
				 "cedar-msvdx: retrying fresh intra after isolated startup abort\n");
			ret = cdv_load_firmware(dev, regs, readl(regs + CDV_MSVDX_REV), job,
						&session->engine);
			if (ret)
				startup_retry_failures++;
		}
		if (!ret && session->engine) {
			session->dev = dev;
			session->regs = regs;
			cdv_active_session = session;
			session_active = 1;
			goto complete; /* Session retains the mapping and runtime-PM reference. */
		}
		if (ret == -EUCLEAN)
			cdv_msvdx_quarantined = true;
		if (ret == -EUCLEAN) {
			engine_power_retained = true;
			goto unlock;
		}
		iounmap(regs);
	}
	pm_runtime_put(dev->dev);
complete:
	if (!ret && session->engine && job->version >= 7) {
		session->config = job->config;
		session->config_valid = true;
	}
	if (!ret) {
		if (job->bound_output)
			session->pending_handle = 0;
		if (session->engine && job->version >= 5)
			session->references = job->next_references;
		else
			cdv_reference_reset(&session->references);
	}
unlock:
	mutex_unlock(&cdv_msvdx_lock);
	return ret ? ret : count;
}

static ssize_t cdv_msvdx_read(struct file *file, char __user *buffer, size_t count,
			      loff_t *position)
{
	struct cdv_session *session = file->private_data;
	struct cdv_job *job;
	ssize_t ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	ret = mutex_lock_interruptible(&cdv_msvdx_lock);
	if (ret)
		return ret;
	job = session->job;
	if (count && job && job->hardware_complete && !job->output) {
		struct cdv_dma_page *pages;
		u32 slot = job->pictures[0].output, base;

		if (cdv_msvdx_quarantined || !session->engine) {
			ret = -EUCLEAN;
			goto done;
		}
		job->output = kvzalloc(job->frame_bytes, GFP_KERNEL);
		if (!job->output) {
			ret = -ENOMEM;
			goto done;
		}
		pages = slot ? session->engine->reference_pages : session->engine->arena;
		base = slot ? (slot - 1) * CDV_SURFACE_SIZE : CDV_H264_OUTPUT_OFFSET;
		cdv_snapshot_at(pages, base, job->output, job->width, job->height);
		job->available = job->frame_bytes;
	}
	ret = job ? simple_read_from_buffer(buffer, count, &session->read_position, job->output,
					    job->available) :
		    -ENODATA;
	*position = session->read_position;
done:
	mutex_unlock(&cdv_msvdx_lock);
	return ret;
}

static long cdv_frame_export_ioctl(struct file *file, void __user *data);
static long cdv_surface_ioctl(struct file *file, void __user *data);

static long cdv_msvdx_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct cedar_msvdx_caps caps = {
		.size = sizeof(caps),
		.version = 1,
		.codec = CDV_CODEC_MODE,
		.width = CDV_H264_WIDTH,
		.height = CDV_H264_HEIGHT,
		.stride = CDV_H264_STRIDE,
		.features = CEDAR_CAP_PRIVILEGED,
		.max_slice_bytes = min_t(u32, 65535, CDV_H264_BITSTREAM_PAGES * PAGE_SIZE),
	};
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (command == CEDAR_MSVDX_EXPORT_FRAME)
		return cdv_frame_export_ioctl(file, (void __user *)argument);
	if (command == CEDAR_MSVDX_SURFACE)
		return cdv_surface_ioctl(file, (void __user *)argument);
	if (command != CEDAR_MSVDX_GET_CAPS)
		return -ENOTTY;
	ret = mutex_lock_interruptible(&cdv_msvdx_lock);
	if (ret)
		return ret;
	if (!cdv_msvdx_dev || cdv_msvdx_quarantined) {
		ret = cdv_msvdx_dev ? -EUCLEAN : -ENODEV;
		goto unlock;
	}
	caps.features |= CEDAR_CAP_PERSISTENT;
	if (cdv_export_layout_valid() && CDV_H264_RUNTIME_TEMPLATE)
		caps.features |= CEDAR_CAP_FRAME_DMABUF;
	if ((caps.features & CEDAR_CAP_FRAME_DMABUF))
		caps.features |= CEDAR_CAP_INDEPENDENT_SURFACES;
	if (CDV_H264_SURFACES == 5 && CDV_H264_COLOCATED_BYTES &&
	    (caps.features & CEDAR_CAP_FRAME_DMABUF))
		caps.features |= CEDAR_CAP_SURFACE_BINDING | CEDAR_CAP_H264_VARIABLE_RESOLUTION;
	caps.features |= CEDAR_CAP_LAZY_READBACK;
	caps.profile_idc = CDV_H264_PROFILE_IDC;
	caps.config_flags = CDV_H264_CONFIG_FLAGS;
	caps.chroma_qp_offset = (s32)CDV_H264_CHROMA_QP;
	caps.second_chroma_qp_offset = (s32)CDV_H264_SECOND_CHROMA_QP;
	caps.disable_deblock = CDV_H264_DISABLE_DEBLOCK;
	caps.alpha = (s32)CDV_H264_ALPHA;
	caps.beta = (s32)CDV_H264_BETA;
	if (CDV_H264_BIDIRECTIONAL)
		caps.features |= CEDAR_CAP_SINGLE_PICTURE;
	if (CDV_H264_RUNTIME_TEMPLATE)
		caps.features |= CEDAR_CAP_H264_RUNTIME;
	if (CDV_H264_RUNTIME_TEMPLATE && CDV_H264_EXPLICIT_P)
		caps.features |= CEDAR_CAP_H264_WEIGHT_P;
	if (CDV_H264_RUNTIME_TEMPLATE && CDV_H264_P_REF2)
		caps.features |= CEDAR_CAP_H264_P_REF2;
	if (CDV_H264_RUNTIME_TEMPLATE && CDV_H264_MULTI_SLICE)
		caps.features |= CEDAR_CAP_H264_SLICES;
	if (CDV_H264_RUNTIME_TEMPLATE && CDV_H264_STABLE_REFS)
		caps.features |= CEDAR_CAP_H264_STABLE_REFS;
	if (CDV_H264_RUNTIME_TEMPLATE && CDV_H264_DPB4)
		caps.features |= CEDAR_CAP_H264_DPB4 | CEDAR_CAP_H264_EXPLICIT_DPB |
				 CEDAR_CAP_H264_FULL_LISTS | CEDAR_CAP_H264_LARGE_SLICES;
	if (caps.features & CEDAR_CAP_H264_LARGE_SLICES)
		caps.max_slice_bytes = CEDAR_H264_MAX_SLICE_BYTES;
	ret = copy_to_user((void __user *)argument, &caps, sizeof(caps)) ? -EFAULT : 0;
unlock:
	mutex_unlock(&cdv_msvdx_lock);
	return ret;
}

static const struct file_operations cdv_msvdx_fops = {
	.owner = THIS_MODULE,
	.open = cdv_msvdx_open,
	.release = cdv_msvdx_release,
	.write = cdv_msvdx_write,
	.read = cdv_msvdx_read,
	.unlocked_ioctl = cdv_msvdx_ioctl,
};

static long cdv_surface_ioctl(struct file *file, void __user *data)
{
	struct cedar_surface_request r;
	struct cdv_session *session = file->private_data;
	struct cdv_surface_storage *storage = NULL;
	u32 index;
	int ret = 0;

	if (copy_from_user(&r, data, sizeof(r)))
		return -EFAULT;
	if (r.version != 1 || r.operation < 1 || r.operation > 5 || r.image_bytes ||
	    r.auxiliary_bytes || r.reserved[0] || r.reserved[1] ||
	    (r.operation == CEDAR_SURFACE_BIND ? r.slot >= CDV_H264_SURFACES : r.slot != U32_MAX) ||
	    ((r.operation == CEDAR_SURFACE_CREATE || r.operation == CEDAR_SURFACE_CANCEL) ?
		     (r.handle || r.id) :
		     (!r.handle || r.handle > CEDAR_SURFACE_HANDLES || !r.id)))
		return -EINVAL;
	if (!cdv_export_layout_valid() || CDV_CODEC_MODE != 1 || !CDV_H264_RUNTIME_TEMPLATE ||
	    CDV_H264_SURFACES != 5 || !CDV_H264_COLOCATED_BYTES)
		return -EOPNOTSUPP;
	mutex_lock(&cdv_msvdx_lock);
	if (!cdv_msvdx_dev || cdv_msvdx_quarantined) {
		ret = -EUCLEAN;
		goto out;
	}
	if (r.operation == CEDAR_SURFACE_CREATE) {
		for (index = 0; index < CEDAR_SURFACE_HANDLES; index++)
			if (!session->handles[index])
				break;
		if (index == CEDAR_SURFACE_HANDLES) {
			ret = -ENOSPC;
			goto out;
		}
		storage = cdv_storage_alloc(cdv_msvdx_dev);
		if (IS_ERR(storage)) {
			ret = PTR_ERR(storage);
			goto out;
		}
		r.handle = index + 1;
		r.id = storage->id;
	} else if (r.operation != CEDAR_SURFACE_CANCEL) {
		index = r.handle - 1;
		storage = session->handles[index];
		if (!storage || storage->id != r.id) {
			ret = -ESTALE;
			goto out;
		}
		if (r.operation == CEDAR_SURFACE_DESTROY && session->pending_handle == r.handle) {
			ret = -EBUSY;
			goto out;
		}
		if (r.operation == CEDAR_SURFACE_BIND &&
		    (session->pending_handle || atomic_read(&storage->readers) ||
		     (cdv_active_session && cdv_active_session != session))) {
			ret = -EBUSY;
			goto out;
		}
	}
	if (storage) {
		r.image_bytes = CEDAR_H264_STORAGE_BYTES;
		r.auxiliary_bytes = CDV_H264_COLOCATED_BYTES;
	}
	if (copy_to_user(data, &r, sizeof(r))) {
		if (r.operation == CEDAR_SURFACE_CREATE)
			kref_put(&storage->refs, cdv_storage_destroy);
		ret = -EFAULT;
		goto out;
	}
	if (r.operation == CEDAR_SURFACE_CREATE) {
		session->handles[index] = storage;
	} else if (r.operation == CEDAR_SURFACE_BIND) {
		session->pending_handle = r.handle;
		session->pending_slot = r.slot;
	} else if (r.operation == CEDAR_SURFACE_CANCEL) {
		session->pending_handle = 0;
	} else if (r.operation == CEDAR_SURFACE_DESTROY) {
		session->handles[index] = NULL;
		kref_put(&storage->refs, cdv_storage_destroy);
	}
out:
	mutex_unlock(&cdv_msvdx_lock);
	return ret;
}

/* Consumes an owning file reference on both success and failure. */
static int cdv_frame_acquire_owned(struct drm_device *dev, struct file *owner,
				   struct cdv_msvdx_frame **out)
{
	struct cdv_session *session;
	struct cdv_job *job;
	struct cdv_dma_page *pages;
	struct cdv_msvdx_frame *frame = NULL;
	u32 slot, base, i;
	int ret = -EINVAL;

	if (!capable(CAP_SYS_RAWIO)) {
		ret = -EPERM;
		goto put;
	}
	if (owner->f_op != &cdv_msvdx_fops)
		goto put;
	mutex_lock(&cdv_msvdx_lock);
	session = owner->private_data;
	job = session->job;
	if (cdv_msvdx_quarantined || cdv_msvdx_dev != dev) {
		ret = -EUCLEAN;
		goto unlock;
	}
	if (cdv_active_session != session || !session->engine || session->dev != dev) {
		ret = -ENODATA;
		goto unlock;
	}

	/* Export the coded rectangle, keeping the complete bounded allocation
	 * and its fixed plane offset in independent-surface mode.
	 */
	if (!job || job->version < 7 || job->frames != 1 || job->flags ||
	    !cedar_h264_picture_geometry_valid(job->width, job->height) ||
	    !cdv_export_layout_valid() || !job->hardware_complete ||
	    (job->output ? job->available != job->frame_bytes : false)) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	slot = job->pictures[0].output;
	if (slot >= CDV_H264_SURFACES)
		goto unlock;
	if (cdv_slot_readers(session->engine, slot)) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!session->engine->surfaces[slot]) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	pages = slot ? session->engine->reference_pages : session->engine->arena;
	base = slot ? (slot - 1) * CDV_SURFACE_SIZE : CDV_H264_OUTPUT_OFFSET;
	if ((base & (PAGE_SIZE - 1)) || !pages)
		goto unlock;
	frame = kzalloc_obj(*frame);
	if (!frame) {
		ret = -ENOMEM;
		goto unlock;
	}
	frame->npages = CEDAR_H264_STORAGE_PAGES;
	frame->cpu_cached = pages[base / PAGE_SIZE].coherent;
	frame->pages = kcalloc(frame->npages, sizeof(*frame->pages), GFP_KERNEL);
	if (!frame->pages) {
		ret = -ENOMEM;
		goto unlock;
	}
	dma_rmb();
	for (i = 0; i < frame->npages; i++) {
		struct cdv_dma_page *page = &pages[base / PAGE_SIZE + i];

		struct cdv_dma_page *owned = &session->engine->surfaces[slot]->pages[i];
		/* The vectors are non-owning mapping aliases. Keep the cached
		 * physical page with storage so rebinding does not lose it.
		 */
		if (owned->cpu != page->cpu || owned->dma != page->dma ||
		    owned->coherent != page->coherent) {
			ret = -EINVAL;
			goto unlock;
		}
		page = owned;

		if (page->coherent != frame->cpu_cached) {
			ret = -EOPNOTSUPP;
			goto unlock;
		}
		if (!page->export_page) {
			struct sg_table sg = {};

			ret = dma_get_sgtable_attrs(dev->dev, &sg, page->cpu, page->dma, PAGE_SIZE,
						    page->coherent ? 0 : DMA_ATTR_WRITE_COMBINE);
			if (ret)
				goto unlock;
			if (sg.orig_nents != 1 || sg.sgl->offset || sg.sgl->length != PAGE_SIZE ||
			    !sg_page(sg.sgl) || upper_32_bits(page_to_phys(sg_page(sg.sgl)))) {
				sg_free_table(&sg);
				ret = -EOPNOTSUPP;
				goto unlock;
			}
			page->export_page = sg_page(sg.sgl);
			sg_free_table(&sg);
		}
		frame->pages[i] = page->export_page;
		drm_clflush_virt_range(page->cpu, PAGE_SIZE);
	}

	frame->storage = session->engine->surfaces[slot];
	kref_get(&frame->storage->refs);
	atomic_inc(&frame->storage->readers);

	frame->dev = dev;
	drm_dev_get(dev);
	frame->id = atomic64_inc_return(&next_frame_id);
	frame->width = job->width;
	frame->height = job->height;
	frame->stride = CEDAR_H264_STORAGE_STRIDE;
	frame->uv_offset = CEDAR_H264_STORAGE_UV_OFFSET;
	frame->slot = slot;
	shared_frames++;
	shared_acquires++;
	if (!frame->verified_bytes)
		unverified_loans++;
	*out = frame;
	mutex_unlock(&cdv_msvdx_lock);
	fput(owner);
	return 0;
unlock:
	if (frame) {
		kfree(frame->pages);
		kfree(frame);
	}
	mutex_unlock(&cdv_msvdx_lock);
put:
	fput(owner);
	return ret;
}

static void cdv_msvdx_frame_release(struct cdv_msvdx_frame *frame)
{
	struct drm_device *dev = frame->dev;
	struct cdv_surface_storage *storage = frame->storage;

	mutex_lock(&cdv_msvdx_lock);
	WARN_ON(atomic_read(&storage->readers) != 1);
	atomic_dec(&storage->readers);
	shared_frames--;
	mutex_unlock(&cdv_msvdx_lock);
	kfree(frame->pages);
	kfree(frame);
	kref_put(&storage->refs, cdv_storage_destroy);
	drm_dev_put(dev);
}

static struct sg_table *cdv_frame_dma_map(struct dma_buf_attachment *attachment,
					  enum dma_data_direction direction)
{
	struct cdv_msvdx_frame *frame = attachment->dmabuf->priv;
	struct sg_table *table;
	struct scatterlist *sg;
	unsigned int i;
	int ret;
	/* Completed pictures are immutable, including while used as references. */
	if (direction != DMA_TO_DEVICE)
		return ERR_PTR(-EACCES);
	table = kzalloc_obj(*table);
	if (!table)
		return ERR_PTR(-ENOMEM);
	ret = sg_alloc_table(table, frame->npages, GFP_KERNEL);
	if (ret) {
		kfree(table);
		return ERR_PTR(ret);
	}
	for_each_sg(table->sgl, sg, frame->npages, i)
		sg_set_page(sg, frame->pages[i], PAGE_SIZE, 0);
	drm_clflush_pages(frame->pages, frame->npages);
	ret = dma_map_sgtable(attachment->dev, table, direction, 0);
	if (ret) {
		sg_free_table(table);
		kfree(table);
		return ERR_PTR(ret);
	}
	return table;
}

static void cdv_frame_dma_unmap(struct dma_buf_attachment *attachment, struct sg_table *table,
				enum dma_data_direction direction)
{
	dma_unmap_sgtable(attachment->dev, table, direction, 0);
	sg_free_table(table);
	kfree(table);
}

static int cdv_frame_dma_begin_cpu(struct dma_buf *buffer, enum dma_data_direction direction)
{
	/* DMA_BUF_SYNC_READ means DMA_FROM_DEVICE for CPU access. */
	if (direction != DMA_FROM_DEVICE)
		return -EACCES;
	/* Acquisition flushes every image page after hardware completion and
	 * before publishing the frame. Readers pin immutable storage: CPU
	 * mappings cannot write, DMA attachments cannot write, and output-slot
	 * reuse is rejected until readers retire. Repeated reads therefore do
	 * not need another full-image invalidation. Retain the ordering barrier.
	 */
	dma_rmb();
	return 0;
}

static int cdv_frame_dma_end_cpu(struct dma_buf *buffer, enum dma_data_direction direction)
{
	return direction == DMA_FROM_DEVICE ? 0 : -EACCES;
}

static int cdv_frame_dma_mmap(struct dma_buf *buffer, struct vm_area_struct *vma)
{
	struct cdv_msvdx_frame *frame = buffer->priv;
	unsigned long pages = vma_pages(vma);
	int ret;

	if (vma->vm_flags & VM_WRITE)
		return -EACCES;
	if (vma->vm_pgoff > frame->npages || pages > frame->npages - vma->vm_pgoff)
		return -EINVAL;
	vm_flags_clear(vma, VM_MAYWRITE);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	/* Match the allocation type: never introduce a WB alias of WC pages.
	 * This driver targets x86, where dma_alloc_coherent uses normal RAM.
	 * Acquisition invalidates image pages before publishing the export.
	 */
	if (!frame->cpu_cached)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	ret = vm_insert_pages(vma, vma->vm_start, frame->pages + vma->vm_pgoff, &pages);
	if (!ret)
		atomic_inc(&frame_mmaps);
	return ret;
}

static void cdv_frame_dma_release(struct dma_buf *buffer)
{
	atomic_dec(&exported_frames);
	cdv_msvdx_frame_release(buffer->priv);
}

static const struct dma_buf_ops cdv_frame_dma_ops = {
	.map_dma_buf = cdv_frame_dma_map,
	.unmap_dma_buf = cdv_frame_dma_unmap,
	.begin_cpu_access = cdv_frame_dma_begin_cpu,
	.end_cpu_access = cdv_frame_dma_end_cpu,
	.mmap = cdv_frame_dma_mmap,
	.release = cdv_frame_dma_release,
};

static bool cdv_frame_request_valid(const struct cedar_frame_export *request, bool info)
{
	return request->version == 1 && !(request->flags & ~(info ? 1U : 0U)) &&
	       !request->reserved && (info ? request->fd >= 0 : request->fd == -1) &&
	       !memchr_inv(&request->id, 0,
			   sizeof(*request) - offsetof(struct cedar_frame_export, id));
}

static void cdv_frame_metadata(struct cedar_frame_export *request, struct cdv_msvdx_frame *frame)
{
	request->id = frame->id;
	request->width = frame->width;
	request->height = frame->height;
	request->stride = frame->stride;
	request->uv_offset = frame->uv_offset;
	request->image_bytes = frame->width * frame->height * 3 / 2;
	request->mapping_bytes = frame->npages * PAGE_SIZE;
	request->fourcc = CEDAR_FRAME_NV12;
	request->slot = frame->slot;
	request->verified_bytes = frame->verified_bytes;
}

static long cdv_frame_export_ioctl(struct file *file, void __user *data)
{
	struct cedar_frame_export request;
	struct cdv_msvdx_frame *frame;
	struct dma_buf *buffer;

	DEFINE_DMA_BUF_EXPORT_INFO(export);
	int fd, ret;

	if (copy_from_user(&request, data, sizeof(request)))
		return -EFAULT;
	if (!cdv_frame_request_valid(&request, false))
		return -EINVAL;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	get_file(file);
	ret = cdv_frame_acquire_owned(READ_ONCE(cdv_msvdx_dev), file, &frame);
	if (ret) {
		put_unused_fd(fd);
		return ret;
	}
	export.exp_name = "cedar-msvdx";
	export.ops = &cdv_frame_dma_ops;
	export.size = frame->npages * PAGE_SIZE;
	export.flags = O_RDONLY;
	export.priv = frame;
	buffer = dma_buf_export(&export);
	if (IS_ERR(buffer)) {
		ret = PTR_ERR(buffer);
		cdv_msvdx_frame_release(frame);
		put_unused_fd(fd);
		return ret;
	}
	atomic_inc(&exported_frames);
	request.fd = fd;
	cdv_frame_metadata(&request, frame);
	if (copy_to_user(data, &request, sizeof(request))) {
		dma_buf_put(buffer);
		put_unused_fd(fd);
		return -EFAULT;
	}
	atomic_inc(&frame_exports);
	dma_buf_fd_install(buffer, fd);
	return 0;
}

static struct miscdevice cdv_msvdx_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cedar-msvdx-test",
	.fops = &cdv_msvdx_fops,
	.mode = 0600,
};

static void cdv_msvdx_cleanup(void *data)
{
	struct drm_device *dev = data;
	bool registered;

	mutex_lock(&cdv_msvdx_lock);
	if (cdv_msvdx_dev != dev) {
		mutex_unlock(&cdv_msvdx_lock);
		return;
	}
	if (cdv_active_session)
		cdv_session_stop(cdv_active_session);
	cdv_msvdx_dev = NULL;
	registered = cdv_msvdx_registered;
	cdv_msvdx_registered = false;
	mutex_unlock(&cdv_msvdx_lock);
	if (registered)
		misc_deregister(&cdv_msvdx_misc);
}

/* gma500 calls this before powering the parent down, after userspace is
 * frozen for system sleep. The synchronous picture path has completed its
 * last write. Retain every allocation and the software DPB for lazy restart.
 */
int cdv_msvdx_power_suspend(struct drm_device *dev)
{
	struct cdv_session *session;
	u32 value;
	int ret = 0;

	/* No live engine needs a checkpoint. Avoid taking the decoder mutex
	 * from runtime suspend while an opener holds it across power_get().
	 * A live engine retains the parent's runtime-PM reference.
	 */
	if (READ_ONCE(cdv_msvdx_dev) != dev)
		return 0;
	if (READ_ONCE(cdv_msvdx_quarantined))
		return -EBUSY;
	if (!READ_ONCE(cdv_active_session))
		return 0;
	mutex_lock(&cdv_msvdx_lock);
	if (cdv_msvdx_dev != dev)
		goto done;
	if (cdv_msvdx_quarantined) {
		ret = -EBUSY;
		goto done;
	}
	session = cdv_active_session;
	if (!session || !session->engine || session->engine->needs_restart)
		goto done;
	writel(0x7f, session->regs + CDV_MSVDX_CLOCK);
	writel(1, session->regs + MTX_SOFT_RESET);
	writel(2, session->regs + MSVDX_MMU_CONTROL);
	ret = readl_poll_timeout(session->regs + MSVDX_MMU_REQUESTS, value, !(value & 255), 10,
				 100000);
	if (!ret)
		ret = cdv_reset_decoder(session->regs);
	if (ret) {
		/* Refuse power-off if quiescence is not established. The owning
		 * session retains its pages and normal quarantine cleanup applies.
		 */
		cdv_msvdx_quarantined = true;
	} else {
		session->engine->mapped = false;
		session->engine->needs_restart = true;
		sleep_checkpoints++;
	}
	dev_info(dev->dev, "cedar-msvdx: stage=sleep_checkpoint result=%d\n", ret);
done:
	mutex_unlock(&cdv_msvdx_lock);
	return ret;
}

void cdv_msvdx_remove(struct drm_device *dev)
{
	if (enable_msvdx)
		devm_release_action(dev->dev, cdv_msvdx_cleanup, dev);
}

int cdv_msvdx_register(struct drm_device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	void __iomem *regs;
	u32 rev;
	int ret;

	if (!enable_msvdx)
		return 0;
	if (pdev->vendor != PCI_VENDOR_ID_INTEL || pdev->device != 0x0be2)
		return -ENODEV;
	if (pci_resource_len(pdev, 0) < CDV_MSVDX_OFFSET + CDV_MSVDX_SIZE)
		return -ENODEV;
	ret = pm_runtime_resume_and_get(dev->dev);
	if (ret < 0)
		return ret;
	regs = ioremap(pci_resource_start(pdev, 0) + CDV_MSVDX_OFFSET, CDV_MSVDX_SIZE);
	if (!regs) {
		ret = -ENOMEM;
		goto power_put;
	}
	rev = readl(regs + CDV_MSVDX_REV);
	iounmap(regs);
	if ((rev & 0xffffff) != 0x020103) {
		ret = -ENODEV;
		goto power_put;
	}
	mutex_lock(&cdv_msvdx_lock);
	if (cdv_msvdx_dev) {
		mutex_unlock(&cdv_msvdx_lock);
		ret = -EBUSY;
		goto power_put;
	}
	cdv_msvdx_dev = dev;
	mutex_unlock(&cdv_msvdx_lock);
	cdv_msvdx_misc.parent = dev->dev;
	ret = misc_register(&cdv_msvdx_misc);
	if (!ret) {
		cdv_msvdx_registered = true;
		ret = devm_add_action_or_reset(dev->dev, cdv_msvdx_cleanup, dev);
	} else {
		cdv_msvdx_dev = NULL;
	}
power_put:
	pm_runtime_put(dev->dev);
	return ret;
}
