/*
 * Copyright © 2012 ARM Limited
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

#include "omap_drmif_fb.h"

struct omap_device {
	int fd;
};

struct omap_bo {
	struct omap_device *dev;
	uint32_t handle;
	uint32_t name;
	uint32_t size;
	void *map_addr;
	uint32_t fb_id;
	uint32_t width;
	uint32_t height;
	uint8_t depth;
	uint8_t bpp;
	uint32_t pitch;
	int refcnt;
};

enum {
	DRM_EXYNOS_GEM_CPU_ACQUIRE_SHARED = 0x0,
	DRM_EXYNOS_GEM_CPU_ACQUIRE_EXCLUSIVE = 0x1,
};

struct drm_exynos_gem_cpu_acquire {
	unsigned int handle;
	unsigned int flags;
};

struct drm_exynos_gem_cpu_release {
	unsigned int handle;
};

/* TODO: use exynos_drm.h kernel headers (http://crosbug.com/37294) */
#define DRM_EXYNOS_GEM_CPU_ACQUIRE	0x08
#define DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_EXYNOS_GEM_CPU_ACQUIRE, struct drm_exynos_gem_cpu_acquire)
#define DRM_EXYNOS_GEM_CPU_RELEASE	0x09
#define DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_EXYNOS_GEM_CPU_RELEASE, struct drm_exynos_gem_cpu_release)

/* device related functions:
 */

struct omap_device *omap_device_new(int fd)
{
	struct omap_device *new_dev = malloc(sizeof(*new_dev));
	if (!new_dev)
		return NULL;

	new_dev->fd = fd;
	return new_dev;
}

void omap_device_del(struct omap_device *dev)
{
	free(dev);
}

/* buffer-object related functions:
 */

struct omap_bo *omap_bo_new_with_dim(struct omap_device *dev,
			uint32_t width, uint32_t height, uint8_t depth,
			uint8_t bpp, uint32_t flags)
{
	struct drm_mode_create_dumb create_dumb;
	struct omap_bo *new_buf;
	int res;

	new_buf = calloc(1, sizeof(*new_buf));
	if (!new_buf)
		return NULL;

	create_dumb.height = height;
	create_dumb.width = width;
	create_dumb.bpp = bpp;
	create_dumb.flags = flags;

	res = drmIoctl(dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
	if (res)
	{
		free(new_buf);
		xf86DrvMsg(-1, X_ERROR, "_CREATE_DUMB("
				"{height: 0x%X, width: 0x%X, bpp: 0x%X, flags: 0x%X}) "
				"failed. errno:0x%X\n",height,width,bpp,flags,errno);
		return NULL;
	}

	new_buf->dev = dev;
	new_buf->handle = create_dumb.handle;
	new_buf->name = 0;
	new_buf->size = create_dumb.size;
	new_buf->map_addr = NULL;
	new_buf->fb_id = 0;
	new_buf->pitch = create_dumb.pitch;

	new_buf->width = create_dumb.width;
	new_buf->height = create_dumb.height;
	new_buf->depth = depth;
	new_buf->bpp = create_dumb.bpp;
	new_buf->refcnt = 1;

	return new_buf;
}

static void omap_bo_del(struct omap_bo *bo)
{
	int res;
	struct drm_mode_destroy_dumb destroy_dumb;

	if (!bo)
		return;

	if (bo->map_addr)
	{
		munmap(bo->map_addr, bo->size);
	}

	if (bo->fb_id)
	{
		res = drmModeRmFB(bo->dev->fd, bo->fb_id);
		assert(res == 0);
	}

	destroy_dumb.handle = bo->handle;
	res = drmIoctl(bo->dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
	assert(res == 0);
	free(bo);

	/* bo->name does not need to be explicitly "released"
	 */
}

void omap_bo_unreference(struct omap_bo *bo)
{
	if (!bo)
		return;

	assert(bo->refcnt > 0);
	if (--bo->refcnt == 0)
		omap_bo_del(bo);
}

void omap_bo_reference(struct omap_bo *bo)
{
	assert(bo->refcnt > 0);
	bo->refcnt++;
}

int omap_bo_get_name(struct omap_bo *bo, uint32_t *name)
{
	if (bo->name == 0) {
		struct drm_gem_flink flink;
		int res;
		flink.handle = bo->handle;
		res = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_FLINK, &flink);
		if (res) {
			xf86DrvMsg(-1, X_ERROR, "_GEM_FLINK("
			    "handle: 0x%X) failed. errno:0x%X\n", flink.handle, errno);
		} else {
			bo->name = flink.name;
		}
	}
	*name = bo->name;
	return 0;
}

uint32_t omap_bo_handle(struct omap_bo *bo)
{
	return bo->handle;
}

uint32_t omap_bo_width(struct omap_bo *bo)
{
	return bo->width;
}

uint32_t omap_bo_height(struct omap_bo *bo)
{
	return bo->height;
}

uint32_t omap_bo_bpp(struct omap_bo *bo)
{
	return bo->bpp;
}

/* Bytes per pixel */
uint32_t omap_bo_Bpp(struct omap_bo *bo)
{
	return (bo->bpp + 7) / 8;
}

uint32_t omap_bo_pitch(struct omap_bo *bo)
{
	return bo->pitch;
}

uint32_t omap_bo_depth(struct omap_bo *bo)
{
	return bo->depth;
}

void *omap_bo_map(struct omap_bo *bo)
{
	if (!bo->map_addr)
	{
		struct drm_mode_map_dumb map_dumb;
		int res;

		map_dumb.handle = bo->handle;

		res = drmIoctl(bo->dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
		if (res)
			return NULL;

		bo->map_addr = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->dev->fd, map_dumb.offset);
	}

	return bo->map_addr;
}

int omap_bo_cpu_prep(struct omap_bo *bo, enum omap_gem_op op)
{
	struct drm_exynos_gem_cpu_acquire acquire;
	int ret;
	acquire.handle = bo->handle;
	if (op == OMAP_GEM_WRITE) {
		acquire.flags = DRM_EXYNOS_GEM_CPU_ACQUIRE_EXCLUSIVE;
	} else {
		acquire.flags = DRM_EXYNOS_GEM_CPU_ACQUIRE_SHARED;
	}
	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE, &acquire);
	if (ret)
		xf86DrvMsg(-1, X_ERROR, "DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE failed: %d", ret);
	return ret;
}

int omap_bo_cpu_fini(struct omap_bo *bo, enum omap_gem_op op)
{
	struct drm_exynos_gem_cpu_release release;
	int ret;
	release.handle = bo->handle;
	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE, &release);
	if (ret)
		xf86DrvMsg(-1, X_ERROR, "DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE failed: %d", ret);
	return ret;
}

int omap_get_param(struct omap_device *dev, uint64_t param, uint64_t *value)
{
	*value = 0x0600;
	return 0;
}

int omap_bo_add_fb(struct omap_bo *bo)
{
	int ret;

	assert(bo->fb_id == 0);

	ret = drmModeAddFB(bo->dev->fd, bo->width, bo->height, bo->depth,
			bo->bpp, bo->pitch, bo->handle, &bo->fb_id);
	if (ret < 0) {
		xf86DrvMsg(-1, X_ERROR, "Could not add fb to bo %d", ret);
		bo->fb_id = 0;
		return ret;
	}
	return 0;
}

uint32_t omap_bo_get_fb(struct omap_bo *bo)
{
	return bo->fb_id;
}

int omap_bo_clear(struct omap_bo *bo)
{
	unsigned char *dst;

	if (!(dst = omap_bo_map(bo))) {
		xf86DrvMsg(-1, X_ERROR,
				"Couldn't map scanout bo\n");
		return -1;
	}
	memset(dst, 0x0, bo->pitch * bo->height);
	return 0;
}
