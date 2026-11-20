// SPDX-License-Identifier: GPL-2.0-only
/*
 *  psb GEM interface
 *
 * Copyright (c) 2011, Intel Corporation.
 *
 * Authors: Alan Cox
 *
 * TODO:
 *	-	we need to work out if the MMU is relevant (eg for
 *		accelerated operations on a GEM object)
 */

#include <linux/iosys-map.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>

#include <asm/set_memory.h>

#include <drm/drm.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <drm/drm_vma_manager.h>

#include "gem.h"
#include "psb_drv.h"

/*
 * PSB GEM object
 */

/*
 * The object's pages, write-combined, for a GART pin or a PRIME export: an
 * exported object needs them, not a slot in the small GTT aperture.
 */
static int psb_gem_get_pages_locked(struct psb_gem_object *pobj)
{
	struct drm_gem_object *obj = &pobj->base;
	struct page **pages;
	int ret;

	dma_resv_assert_held(obj->resv);

	if (pobj->pages_use)
		goto out;
	pages = drm_gem_get_pages(obj);
	if (IS_ERR(pages))
		return PTR_ERR(pages);
	ret = set_pages_array_wc(pages, obj->size / PAGE_SIZE);
	if (ret) {
		drm_gem_put_pages(obj, pages, false, false);
		return ret;
	}
	pobj->pages = pages;
out:
	pobj->pages_use++;
	return 0;
}

static void psb_gem_put_pages_locked(struct psb_gem_object *pobj)
{
	struct drm_gem_object *obj = &pobj->base;

	dma_resv_assert_held(obj->resv);

	if (WARN_ON(!pobj->pages_use) || --pobj->pages_use)
		return;
	/* Reset caching flags */
	set_pages_array_wb(pobj->pages, obj->size / PAGE_SIZE);
	drm_gem_put_pages(obj, pobj->pages, true, false);
	pobj->pages = NULL;
}

int psb_gem_pin(struct psb_gem_object *pobj)
{
	struct drm_gem_object *obj = &pobj->base;
	struct drm_device *dev = obj->dev;
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	u32 gpu_base = dev_priv->gtt.gatt_start;
	unsigned int npages;
	int ret;

	ret = dma_resv_lock(obj->resv, NULL);
	if (drm_WARN_ONCE(dev, ret, "dma_resv_lock() failed, ret=%d\n", ret))
		return ret;

	if (pobj->in_gart || pobj->stolen)
		goto out; /* already mapped */

	ret = psb_gem_get_pages_locked(pobj);
	if (ret)
		goto err_dma_resv_unlock;

	npages = obj->size / PAGE_SIZE;

	psb_gtt_insert_pages(dev_priv, &pobj->resource, pobj->pages);
	if (dev_priv->mmu)
		psb_mmu_insert_pages(psb_mmu_get_default_pd(dev_priv->mmu), pobj->pages,
				     (gpu_base + pobj->offset), npages, 0, 0,
				     PSB_MMU_CACHED_MEMORY);

out:
	++pobj->in_gart;
	dma_resv_unlock(obj->resv);

	return 0;

err_dma_resv_unlock:
	dma_resv_unlock(obj->resv);
	return ret;
}

void psb_gem_unpin(struct psb_gem_object *pobj)
{
	struct drm_gem_object *obj = &pobj->base;
	struct drm_device *dev = obj->dev;
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	u32 gpu_base = dev_priv->gtt.gatt_start;
	unsigned long npages;
	int ret;

	ret = dma_resv_lock(obj->resv, NULL);
	if (drm_WARN_ONCE(dev, ret, "dma_resv_lock() failed, ret=%d\n", ret))
		return;

	WARN_ON(!pobj->in_gart);

	--pobj->in_gart;

	if (pobj->in_gart || pobj->stolen)
		goto out;

	npages = obj->size / PAGE_SIZE;

	if (dev_priv->mmu)
		psb_mmu_remove_pages(psb_mmu_get_default_pd(dev_priv->mmu),
				     (gpu_base + pobj->offset), npages, 0, 0);
	psb_gtt_remove_pages(dev_priv, &pobj->resource);

	psb_gem_put_pages_locked(pobj);

out:
	dma_resv_unlock(obj->resv);
}

static vm_fault_t psb_gem_fault(struct vm_fault *vmf);

static void psb_gem_free_object(struct drm_gem_object *obj)
{
	struct psb_gem_object *pobj = to_psb_gem_object(obj);

	/* Undo the mmap pin if we are destroying the object */
	if (pobj->mmapping)
		psb_gem_unpin(pobj);

	drm_gem_object_release(obj);

	WARN_ON(pobj->in_gart && !pobj->stolen);

	release_resource(&pobj->resource);
	kfree(pobj);
}

static const struct vm_operations_struct psb_gem_vm_ops = {
	.fault = psb_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/*
 * PRIME export, for a render GPU to draw into scanout buffers. Only objects
 * with pages can be shared: stolen memory has none. An export pins the pages,
 * write-combined, and doesn't insert them into the GTT. The PRIME helpers
 * call pin and unpin with the reservation lock held.
 */
static int psb_gem_prime_pin(struct drm_gem_object *obj)
{
	struct psb_gem_object *pobj = to_psb_gem_object(obj);

	if (pobj->stolen)
		return -EINVAL;
	return psb_gem_get_pages_locked(pobj);
}

static void psb_gem_prime_unpin(struct drm_gem_object *obj)
{
	psb_gem_put_pages_locked(to_psb_gem_object(obj));
}

static struct sg_table *psb_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct psb_gem_object *pobj = to_psb_gem_object(obj);

	if (!pobj->pages)
		return ERR_PTR(-EINVAL);
	return drm_prime_pages_to_sg(obj->dev, pobj->pages, obj->size >> PAGE_SHIFT);
}

/*
 * A kernel mapping for an importer, write-combined like the pages. It holds
 * the pages as a PRIME pin does. The PRIME helpers call it with the
 * reservation lock held.
 */
static int psb_gem_prime_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct psb_gem_object *pobj = to_psb_gem_object(obj);
	void *vaddr;
	int ret;

	if (pobj->stolen)
		return -EINVAL;
	ret = psb_gem_get_pages_locked(pobj);
	if (ret)
		return ret;
	vaddr = vmap(pobj->pages, obj->size >> PAGE_SHIFT, VM_MAP,
		     pgprot_writecombine(PAGE_KERNEL));
	if (!vaddr) {
		psb_gem_put_pages_locked(pobj);
		return -ENOMEM;
	}
	iosys_map_set_vaddr(map, vaddr);
	return 0;
}

static void psb_gem_prime_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	vunmap(map->vaddr);
	psb_gem_put_pages_locked(to_psb_gem_object(obj));
}

/*
 * dma_buf_mmap() maps through the object's fake offset (drm_gem_prime_mmap()),
 * which a dumb buffer only gets from MAP_DUMB: make sure it has one.
 */
static struct dma_buf *psb_gem_prime_export(struct drm_gem_object *obj, int flags)
{
	int ret;

	/* No struct pages to share. */
	if (to_psb_gem_object(obj)->stolen)
		return ERR_PTR(-EINVAL);
	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		return ERR_PTR(ret);
	return drm_gem_prime_export(obj, flags);
}

static const struct drm_gem_object_funcs psb_gem_object_funcs = {
	.free = psb_gem_free_object,
	.export = psb_gem_prime_export,
	.pin = psb_gem_prime_pin,
	.unpin = psb_gem_prime_unpin,
	.get_sg_table = psb_gem_prime_get_sg_table,
	.vmap = psb_gem_prime_vmap,
	.vunmap = psb_gem_prime_vunmap,
	.vm_ops = &psb_gem_vm_ops,
};

struct psb_gem_object *
psb_gem_create(struct drm_device *dev, u64 size, const char *name, bool stolen, u32 align)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct psb_gem_object *pobj;
	struct drm_gem_object *obj;
	int ret;

	size = roundup(size, PAGE_SIZE);

	pobj = kzalloc_obj(*pobj);
	if (!pobj)
		return ERR_PTR(-ENOMEM);
	obj = &pobj->base;

	/* GTT resource */

	ret = psb_gtt_allocate_resource(dev_priv, &pobj->resource, name, size, align, stolen,
					&pobj->offset);
	if (ret)
		goto err_kfree;

	if (stolen) {
		pobj->stolen = true;
		pobj->in_gart = 1;
	}

	/* GEM object */

	obj->funcs = &psb_gem_object_funcs;

	if (stolen) {
		drm_gem_private_object_init(dev, obj, size);
	} else {
		ret = drm_gem_object_init(dev, obj, size);
		if (ret)
			goto err_release_resource;

		/* Limit the object to 32-bit mappings */
		mapping_set_gfp_mask(obj->filp->f_mapping, GFP_KERNEL | __GFP_DMA32);
	}

	return pobj;

err_release_resource:
	release_resource(&pobj->resource);
err_kfree:
	kfree(pobj);
	return ERR_PTR(ret);
}

/**
 *	psb_gem_dumb_create	-	create a dumb buffer
 *	@file: our client file
 *	@dev: our device
 *	@args: the requested arguments copied from userspace
 *
 *	Allocate a buffer suitable for use for a frame buffer of the
 *	form described by user space. Give userspace a handle by which
 *	to reference it.
 */
int psb_gem_dumb_create(struct drm_file *file, struct drm_device *dev,
			struct drm_mode_create_dumb *args)
{
	size_t pitch, size;
	struct psb_gem_object *pobj;
	struct drm_gem_object *obj;
	u32 handle;
	int ret;

	pitch = args->width * DIV_ROUND_UP(args->bpp, 8);
	pitch = ALIGN(pitch, 64);

	size = pitch * args->height;
	size = roundup(size, PAGE_SIZE);
	if (!size)
		return -EINVAL;

	pobj = psb_gem_create(dev, size, "gem", false, PAGE_SIZE);
	if (IS_ERR(pobj))
		return PTR_ERR(pobj);
	obj = &pobj->base;

	ret = drm_gem_handle_create(file, obj, &handle);
	if (ret)
		goto err_drm_gem_object_put;

	drm_gem_object_put(obj);

	args->pitch = pitch;
	args->size = size;
	args->handle = handle;

	return 0;

err_drm_gem_object_put:
	drm_gem_object_put(obj);
	return ret;
}

/**
 *	psb_gem_fault		-	pagefault handler for GEM objects
 *	@vmf: fault detail
 *
 *	Invoked when a fault occurs on an mmap of a GEM managed area. GEM
 *	does most of the work for us including the actual map/unmap calls
 *	but we need to do the actual page work.
 *
 *	This code eventually needs to handle faulting objects in and out
 *	of the GTT and repacking it when we run out of space. We can put
 *	that off for now and for our simple uses
 *
 *	The VMA was set up by GEM. In doing so it also ensured that the
 *	vma->vm_private_data points to the GEM object that is backing this
 *	mapping.
 */
static vm_fault_t psb_gem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *obj;
	struct psb_gem_object *pobj;
	int err;
	vm_fault_t ret;
	unsigned long pfn;
	pgoff_t page_offset;
	struct drm_device *dev;
	struct drm_psb_private *dev_priv;

	obj = vma->vm_private_data;	/* GEM object */
	dev = obj->dev;
	dev_priv = to_drm_psb_private(dev);

	pobj = to_psb_gem_object(obj);

	/* Make sure we don't parallel update on a fault, nor move or remove
	   something from beneath our feet */
	mutex_lock(&dev_priv->mmap_mutex);

	/* For now the mmap pins the object and it stays pinned. As things
	   stand that will do us no harm */
	if (pobj->mmapping == 0) {
		err = psb_gem_pin(pobj);
		if (err < 0) {
			dev_err(dev->dev, "gma500: pin failed: %d\n", err);
			ret = vmf_error(err);
			goto fail;
		}
		pobj->mmapping = 1;
	}

	/* Page relative to the VMA start - we must calculate this ourselves
	   because vmf->pgoff is the fake GEM offset */
	page_offset = linear_page_delta(vma, vmf->address);

	/* CPU view of the page, don't go via the GART for CPU writes */
	if (pobj->stolen)
		pfn = (dev_priv->stolen_base + pobj->offset) >> PAGE_SHIFT;
	else
		pfn = page_to_pfn(pobj->pages[page_offset]);
	ret = vmf_insert_pfn(vma, vmf->address, pfn);
fail:
	mutex_unlock(&dev_priv->mmap_mutex);

	return ret;
}

/*
 * Memory management
 */

/* Insert vram stolen pages into the GTT. */
static void psb_gem_mm_populate_stolen(struct drm_psb_private *pdev)
{
	struct drm_device *dev = &pdev->dev;
	unsigned int pfn_base;
	unsigned int i, num_pages;
	uint32_t pte;

	pfn_base = pdev->stolen_base >> PAGE_SHIFT;
	num_pages = pdev->vram_stolen_size >> PAGE_SHIFT;

	drm_dbg(dev, "Set up %u stolen pages starting at 0x%08x, GTT offset %dK\n",
		num_pages, pfn_base << PAGE_SHIFT, 0);

	for (i = 0; i < num_pages; ++i) {
		pte = psb_gtt_mask_pte(pfn_base + i, PSB_MMU_CACHED_MEMORY);
		iowrite32(pte, pdev->gtt_map + i);
	}

	(void)ioread32(pdev->gtt_map + i - 1);
}

int psb_gem_mm_init(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	unsigned long stolen_size, vram_stolen_size;
	struct psb_gtt *pg;
	int ret;

	mutex_init(&dev_priv->mmap_mutex);

	pg = &dev_priv->gtt;

	pci_read_config_dword(pdev, PSB_BSM, &dev_priv->stolen_base);
	vram_stolen_size = pg->gtt_phys_start - dev_priv->stolen_base - PAGE_SIZE;

	stolen_size = vram_stolen_size;

	dev_dbg(dev->dev, "Stolen memory base 0x%x, size %luK\n",
		dev_priv->stolen_base, vram_stolen_size / 1024);

	pg->stolen_size = stolen_size;
	dev_priv->vram_stolen_size = vram_stolen_size;

	dev_priv->vram_addr = ioremap_wc(dev_priv->stolen_base, stolen_size);
	if (!dev_priv->vram_addr) {
		dev_err(dev->dev, "Failure to map stolen base.\n");
		ret = -ENOMEM;
		goto err_mutex_destroy;
	}

	psb_gem_mm_populate_stolen(dev_priv);

	return 0;

err_mutex_destroy:
	mutex_destroy(&dev_priv->mmap_mutex);
	return ret;
}

void psb_gem_mm_fini(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);

	iounmap(dev_priv->vram_addr);

	mutex_destroy(&dev_priv->mmap_mutex);
}

/* Re-insert all pinned GEM objects into GTT. */
static void psb_gem_mm_populate_resources(struct drm_psb_private *pdev)
{
	unsigned int restored = 0, total = 0, size = 0;
	struct resource *r = pdev->gtt_mem->child;
	struct drm_device *dev = &pdev->dev;
	struct psb_gem_object *pobj;

	while (r) {
		/*
		 * TODO: GTT restoration needs a refactoring, so that we don't have to touch
		 *       struct psb_gem_object here. The type represents a GEM object and is
		 *       not related to the GTT itself.
		 */
		pobj = container_of(r, struct psb_gem_object, resource);
		/* Pinned into the GTT, not only exported or mapped. */
		if (pobj->in_gart && pobj->pages) {
			psb_gtt_insert_pages(pdev, &pobj->resource, pobj->pages);
			size += resource_size(&pobj->resource);
			++restored;
		}
		r = r->sibling;
		++total;
	}

	drm_dbg(dev, "Restored %u of %u gtt ranges (%u KB)", restored, total, (size / 1024));
}

int psb_gem_mm_resume(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = to_drm_psb_private(dev);
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	unsigned long stolen_size, vram_stolen_size;
	struct psb_gtt *pg;

	pg = &dev_priv->gtt;

	pci_read_config_dword(pdev, PSB_BSM, &dev_priv->stolen_base);
	vram_stolen_size = pg->gtt_phys_start - dev_priv->stolen_base - PAGE_SIZE;

	stolen_size = vram_stolen_size;

	dev_dbg(dev->dev, "Stolen memory base 0x%x, size %luK\n", dev_priv->stolen_base,
		vram_stolen_size / 1024);

	if (stolen_size != pg->stolen_size) {
		dev_err(dev->dev, "GTT resume error.\n");
		return -EINVAL;
	}

	psb_gem_mm_populate_stolen(dev_priv);
	psb_gem_mm_populate_resources(dev_priv);

	return 0;
}
