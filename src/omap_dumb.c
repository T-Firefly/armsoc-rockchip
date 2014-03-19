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

#include "omap_dumb.h"
#include "omap_msg.h"

struct omap_device {
	int fd;
	ScrnInfoPtr pScrn;
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
	int acquired_exclusive;
	int acquire_cnt;
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

struct omap_device *omap_device_new(int fd, ScrnInfoPtr pScrn)
{
	struct omap_device *new_dev = calloc(1, sizeof *new_dev);
	if (!new_dev)
		return NULL;

	new_dev->fd = fd;
	new_dev->pScrn = pScrn;

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
	ScrnInfoPtr pScrn = dev->pScrn;
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
		ERROR_MSG("CREATE_DUMB(%ux%u bpp: %u flags: 0x%x) failed: %s",
				height, width, bpp, flags, strerror(errno));
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
	new_buf->acquired_exclusive = 0;
	new_buf->acquire_cnt = 0;

	DEBUG_MSG("[BO:%u] Created (%dx%d bpp: %u flags: 0x%x) => pitch: %u size: %llu",
			create_dumb.handle, create_dumb.width,
			create_dumb.height, create_dumb.bpp, create_dumb.flags,
			create_dumb.pitch, create_dumb.size);

	return new_buf;
}

static void omap_bo_del(struct omap_bo *bo)
{
	ScrnInfoPtr pScrn;
	int res;
	struct drm_mode_destroy_dumb destroy_dumb;

	if (!bo)
		return;

	pScrn = bo->dev->pScrn;

	DEBUG_MSG("[BO:%u] [FB:%u] [FLINK:%u] mmap: %p size: %u",
			bo->handle, bo->fb_id, bo->name, bo->map_addr,
			bo->size);

	if (bo->map_addr)
	{
		res = munmap(bo->map_addr, bo->size);
		if (res)
			ERROR_MSG("[BO:%u] munmap(%u) failed: %s",
					bo->handle, bo->size, strerror(errno));
		assert(res == 0);
	}

	if (bo->fb_id)
	{
		res = drmModeRmFB(bo->dev->fd, bo->fb_id);
		if (res)
			ERROR_MSG("[BO:%u] Remove [FB:%u] failed: %s",
					bo->handle, bo->fb_id, strerror(errno));
		assert(res == 0);
	}

	destroy_dumb.handle = bo->handle;
	res = drmIoctl(bo->dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
	if (res)
		ERROR_MSG("[BO:%u] DESTROY_DUMB failed: %s",
				bo->handle, strerror(errno));
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

uint32_t omap_bo_get_name(struct omap_bo *bo)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_gem_flink flink;
	int ret;

	if (bo->name)
		return bo->name;

	flink.handle = bo->handle;
	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_FLINK, &flink);
	if (ret) {
		ERROR_MSG("[BO:%u] GEM_FLINK failed: %s",
				bo->handle, strerror(errno));
		return 0;
	}

	bo->name = flink.name;
	DEBUG_MSG("[BO:%u] [FB:%u] [FLINK:%u] mmap: %p",
			bo->handle, bo->fb_id, bo->name, bo->map_addr);

	return bo->name;
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
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_mode_map_dumb map_dumb;
	int res;
	void *map_addr;

	if (bo->map_addr)
		return bo->map_addr;

	map_dumb.handle = bo->handle;

	res = drmIoctl(bo->dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
	if (res) {
		ERROR_MSG("[BO:%u] MODE_MAP_DUMB failed: %s",
				bo->handle, strerror(errno));
		return NULL;
	}

	map_addr = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			bo->dev->fd, map_dumb.offset);
	if (map_addr == MAP_FAILED) {
		ERROR_MSG("[BO:%u] mmap bo failed: %s",
				bo->handle, strerror(errno));
		return NULL;
	}

	bo->map_addr = map_addr;
	DEBUG_MSG("[BO:%u] [FB:%u] [FLINK:%u] mmap: %p mapped %u bytes",
			bo->handle, bo->fb_id, bo->name, bo->map_addr,
			bo->size);

	return bo->map_addr;
}

int omap_bo_cpu_prep(struct omap_bo *bo, enum omap_gem_op op)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_exynos_gem_cpu_acquire acquire;
	int ret;

	if (bo->acquire_cnt) {
		if ((op & OMAP_GEM_WRITE) && !bo->acquired_exclusive) {
			ERROR_MSG("attempting to acquire read locked surface for write");
			return 1;
		}
		bo->acquire_cnt++;
		return 0;
	}
	bo->acquired_exclusive = op & OMAP_GEM_WRITE;
	bo->acquire_cnt++;
	acquire.handle = bo->handle;
	acquire.flags = (op & OMAP_GEM_WRITE)
		? DRM_EXYNOS_GEM_CPU_ACQUIRE_EXCLUSIVE
		: DRM_EXYNOS_GEM_CPU_ACQUIRE_SHARED;
	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE, &acquire);
	if (ret)
		ERROR_MSG("DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE failed: %s",
				strerror(errno));
	return ret;
}

int omap_bo_cpu_fini(struct omap_bo *bo, enum omap_gem_op op)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_exynos_gem_cpu_release release;
	int ret;

	assert(bo->acquire_cnt > 0);

	if (--bo->acquire_cnt != 0) {
		return 0;
	}
	release.handle = bo->handle;
	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE, &release);
	if (ret)
		ERROR_MSG("DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE failed: %s",
				strerror(errno));
	return ret;
}

uint32_t omap_bo_get_fb(struct omap_bo *bo)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	int ret;
	uint32_t fb_id;

	if (bo->fb_id)
		return bo->fb_id;

	ret = drmModeAddFB(bo->dev->fd, bo->width, bo->height, bo->depth,
			bo->bpp, bo->pitch, bo->handle, &fb_id);
	if (ret < 0) {
		ERROR_MSG("[BO:%u] add FB (%ux%u pitch:%u bpp:%u depth:%u) failed: %s",
				bo->handle, bo->width, bo->height, bo->pitch,
				bo->bpp, bo->depth, strerror(errno));
		return 0;
	}

	bo->fb_id = fb_id;
	DEBUG_MSG("[BO:%u] [FB:%u] [FLINK:%u] mmap: %p Added FB: %ux%u depth: %u bpp: %u pitch: %u",
			bo->handle, bo->fb_id, bo->name, bo->map_addr,
			bo->width, bo->height, bo->depth, bo->bpp, bo->pitch);

	return bo->fb_id;
}

int omap_bo_clear(struct omap_bo *bo)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	unsigned char *dst;

	if (!(dst = omap_bo_map(bo))) {
		ERROR_MSG("[BO:%u] Could not map scanout\n", bo->handle);
		return -1;
	}
	memset(dst, 0x0, bo->pitch * bo->height);
	return 0;
}
