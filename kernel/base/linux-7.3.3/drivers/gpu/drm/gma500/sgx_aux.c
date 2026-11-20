// SPDX-License-Identifier: GPL-2.0-only
/*
 * The SGX of a GMA500-family GPU, published as an auxiliary device for a
 * driver of its own on the chips whose psb_ops name it (Cedarview). gma500
 * keeps the PCI function and the interrupt line, hands the SGX's interrupt
 * over, and never sets the SGX up or touches its registers on those chips.
 *
 * Copyright © 2026 Joseph Telaak
 */

#include <linux/pci.h>
#include <linux/slab.h>

#include <drm/drm_print.h>
#include <drm/gma500_sgx.h>

#include "psb_drv.h"
#include "psb_irq.h"

struct gma_sgx_aux {
	struct gma500_sgx_device sgx;
	struct drm_psb_private *dev_priv;
};

static int gma_sgx_set_irq_handler(struct gma500_sgx_device *sgx,
				   gma500_sgx_irq_handler_t handler, void *data)
{
	struct gma_sgx_aux *aux = container_of(sgx, struct gma_sgx_aux, sgx);
	struct drm_psb_private *dev_priv = aux->dev_priv;
	struct pci_dev *pdev = to_pci_dev(dev_priv->dev.dev);
	unsigned long flags;

	spin_lock_irqsave(&dev_priv->irqmask_lock, flags);
	if (handler && dev_priv->sgx_handler) {
		spin_unlock_irqrestore(&dev_priv->irqmask_lock, flags);
		return -EBUSY;
	}
	if (handler)
		WRITE_ONCE(dev_priv->sgx_data, data);
	/* Pairs with gma_irq_handler(): the data reaches it before the handler. */
	smp_store_release(&dev_priv->sgx_handler, handler);
	/* The SGX's interrupt reaches the CPU only while a handler takes it. */
	gma_sgx_irq_update(dev_priv);
	spin_unlock_irqrestore(&dev_priv->irqmask_lock, flags);

	if (!handler) {
		/* No handler call still running on another CPU, then the data goes. */
		if (dev_priv->irq_enabled)
			synchronize_irq(pdev->irq);
		WRITE_ONCE(dev_priv->sgx_data, NULL);
	}
	return 0;
}

static void gma_sgx_aux_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);

	kfree(container_of(to_gma500_sgx_device(adev), struct gma_sgx_aux, sgx));
}

void gma_sgx_aux_init(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct gma_sgx_aux *aux;
	int ret;

	aux = kzalloc_obj(*aux);
	if (!aux) {
		drm_warn(dev, "SGX auxiliary device not published (-ENOMEM)\n");
		return;
	}
	aux->dev_priv = dev_priv;
	aux->sgx.regs = dev_priv->sgx_reg;
	aux->sgx.regs_len = PSB_SGX_SIZE;
	aux->sgx.core = dev_priv->ops->sgx->core;
	aux->sgx.set_irq_handler = gma_sgx_set_irq_handler;
	aux->sgx.adev.name = "sgx";
	aux->sgx.adev.id = pci_dev_id(to_pci_dev(dev->dev));
	aux->sgx.adev.dev.parent = dev->dev;
	aux->sgx.adev.dev.release = gma_sgx_aux_release;

	ret = auxiliary_device_init(&aux->sgx.adev);
	if (ret) {
		kfree(aux);
		drm_warn(dev, "SGX auxiliary device not published (%d)\n", ret);
		return;
	}
	ret = auxiliary_device_add(&aux->sgx.adev);
	if (ret) {
		auxiliary_device_uninit(&aux->sgx.adev);
		drm_warn(dev, "SGX auxiliary device not published (%d)\n", ret);
		return;
	}
	dev_priv->sgx_adev = &aux->sgx;
}

void gma_sgx_aux_fini(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	if (!dev_priv->sgx_adev)
		return;
	/* Unbinding the SGX driver hands the interrupt back; make sure of it. */
	auxiliary_device_delete(&dev_priv->sgx_adev->adev);
	gma_sgx_set_irq_handler(dev_priv->sgx_adev, NULL, NULL);
	auxiliary_device_uninit(&dev_priv->sgx_adev->adev);
	dev_priv->sgx_adev = NULL;
}
