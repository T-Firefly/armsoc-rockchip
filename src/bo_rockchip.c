/*
 * Copyright © 2014 ROCKCHIP, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#include <stdlib.h>
#include <sys/mman.h>
#include <assert.h>
#include <errno.h>
#include <xf86.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/rockchip_drmif.h>
#include "omap_dumb.h"
#include "omap_msg.h"

enum {
	DRM_ROCKCHIP_GEM_CPU_ACQUIRE_SHARED = 0x0,
	DRM_ROCKCHIP_GEM_CPU_ACQUIRE_EXCLUSIVE = 0x1,
};

struct drm_rockchip_gem_cpu_acquire {
	uint32_t handle;
	uint32_t flags;
};

struct drm_rockchip_gem_cpu_release {
	uint32_t handle;
};

/* TODO: use rockchip_drm.h kernel headers */
#define DRM_ROCKCHIP_GEM_CPU_ACQUIRE     0x02
#define DRM_IOCTL_ROCKCHIP_GEM_CPU_ACQUIRE       DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ROCKCHIP_GEM_CPU_ACQUIRE, struct drm_rockchip_gem_cpu_acquire)
#define DRM_ROCKCHIP_GEM_CPU_RELEASE     0x03
#define DRM_IOCTL_ROCKCHIP_GEM_CPU_RELEASE       DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_ROCKCHIP_GEM_CPU_RELEASE, struct drm_rockchip_gem_cpu_release)



static void *bo_rockchip_create(struct omap_device *dev,
			size_t size, uint32_t flags, uint32_t *handle)
{
	struct rockchip_bo *rockchip_bo;

	rockchip_bo = rockchip_bo_create(dev->bo_dev, size, flags);
	*handle = rockchip_bo_handle(rockchip_bo);

	return rockchip_bo;
}

static void bo_rockchip_destroy(struct omap_bo *bo)
{
	rockchip_bo_destroy(bo->priv_bo);
}

static int bo_rockchip_get_name(struct omap_bo *bo, uint32_t *name)
{
	return rockchip_bo_get_name(bo->priv_bo, name);
}

static void *bo_rockchip_map(struct omap_bo *bo)
{
	struct rockchip_bo *rockchip_bo = bo->priv_bo;
	if (rockchip_bo->vaddr)
		return rockchip_bo->vaddr;
	return rockchip_bo_map(bo->priv_bo);
}

static int bo_rockchip_cpu_prep(struct omap_bo *bo, enum omap_gem_op op)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_rockchip_gem_cpu_acquire acquire;
	int ret;

	acquire.handle = bo->handle;
	acquire.flags = (op & OMAP_GEM_WRITE)
		? DRM_ROCKCHIP_GEM_CPU_ACQUIRE_EXCLUSIVE
		: DRM_ROCKCHIP_GEM_CPU_ACQUIRE_SHARED;
	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_ROCKCHIP_GEM_CPU_ACQUIRE,
			&acquire);
	if (ret)
		ERROR_MSG("DRM_IOCTL_ROCKCHIP_GEM_CPU_ACQUIRE failed: %s",
				strerror(errno));
	return ret;
}

static int bo_rockchip_cpu_fini(struct omap_bo *bo, enum omap_gem_op op)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_rockchip_gem_cpu_release release;
	int ret;

	release.handle = bo->handle;
	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_ROCKCHIP_GEM_CPU_RELEASE,
			&release);
	if (ret)
		ERROR_MSG("DRM_IOCTL_ROCKCHIP_GEM_CPU_RELEASE failed: %s",
				strerror(errno));
	return ret;
}

static const struct bo_ops bo_rockchip_ops = {
	.bo_create = bo_rockchip_create,
	.bo_destroy = bo_rockchip_destroy,
	.bo_get_name = bo_rockchip_get_name,
	.bo_map = bo_rockchip_map,
	.bo_cpu_prep = bo_rockchip_cpu_prep,
	.bo_cpu_fini = bo_rockchip_cpu_fini,
};

int bo_device_init(struct omap_device *dev)
{
	struct rockchip_device *new_rockchip_dev;

	new_rockchip_dev = rockchip_device_create(dev->fd);
	if (!new_rockchip_dev)
		return FALSE;

	dev->bo_dev = new_rockchip_dev;
	dev->ops = &bo_rockchip_ops;

	return TRUE;
}

void bo_device_deinit(struct omap_device *dev)
{
	if (dev->bo_dev)
		rockchip_device_destroy(dev->bo_dev);
}
