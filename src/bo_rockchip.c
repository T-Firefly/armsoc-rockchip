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
	return 0;
}

static int bo_rockchip_cpu_fini(struct omap_bo *bo, enum omap_gem_op op)
{
	return 0;
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
