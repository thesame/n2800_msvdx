/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CDV_MSVDX_H
#define CDV_MSVDX_H

struct drm_device;

int cdv_msvdx_register(struct drm_device *dev);
void cdv_msvdx_remove(struct drm_device *dev);
int cdv_msvdx_power_suspend(struct drm_device *dev);

#endif
