/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

#ifndef __DRM_GMA500_SGX_H__
#define __DRM_GMA500_SGX_H__

#include <linux/auxiliary_bus.h>
#include <linux/interrupt.h>
#include <linux/types.h>

/* The SGX's interrupt handler: hard interrupt context. */
typedef irqreturn_t (*gma500_sgx_irq_handler_t)(void *data);

/**
 * struct gma500_sgx_device - The PowerVR SGX inside a GMA500-family GPU
 *
 * gma500 owns the PCI function, its register BAR and its interrupt. On parts
 * whose SGX has a driver of its own (&psb_ops.sgx), it publishes this auxiliary
 * device, named "gma500_gfx.sgx", whose parent is the GPU's PCI device. The SGX
 * driver uses the parent for DMA and runtime PM: holding the auxiliary device
 * awake keeps gma500, and so the graphics power island, awake.
 */
struct gma500_sgx_device {
	/** @adev: The auxiliary device. */
	struct auxiliary_device adev;

	/** @regs: The SGX's register window, mapped by gma500. */
	void __iomem *regs;

	/** @regs_len: Length of @regs in bytes. */
	resource_size_t regs_len;

	/** @core: Which SGX core this is, by number: 545 for an SGX545. */
	unsigned int core;

	/**
	 * @set_irq_handler: Route the SGX's interrupts to @handler, which runs in
	 * hard interrupt context with @data; with a NULL @handler they are
	 * masked in the display controller, and once it returns no call to the
	 * old handler is still running. gma500 never writes the registers of an
	 * SGX it publishes.
	 * Returns -EBUSY if another handler is set.
	 */
	int (*set_irq_handler)(struct gma500_sgx_device *sgx,
			       gma500_sgx_irq_handler_t handler, void *data);
};

static inline struct gma500_sgx_device *
to_gma500_sgx_device(struct auxiliary_device *adev)
{
	return container_of(adev, struct gma500_sgx_device, adev);
}

#endif /* __DRM_GMA500_SGX_H__ */
