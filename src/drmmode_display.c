/*
 * Copyright © 2007 Red Hat, Inc.
 * Copyright © 2008 Maarten Maathuis
 * Copyright © 2011 Texas Instruments, Inc
 *
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
 * Authors:
 *    Dave Airlie <airlied@redhat.com>
 *    Ian Elliott <ianelliottus@yahoo.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* TODO cleanup #includes, remove unnecessary ones */

#include "xorg-server.h"
#include "xorgVersion.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include <linux/fb.h>


/* All drivers should typically include these */
#include "xf86.h"
#include "xf86_OSproc.h"
#define PPC_MMIO_IS_BE
#include "compiler.h"
#include "mipointer.h"

/* All drivers implementing backing store need this */
#include "mibstore.h"

#include "micmap.h"

#include "xf86DDC.h"

#include "xf86RandR12.h"
#include "dixstruct.h"
#include "scrnintstr.h"
#include "fb.h"
#include "xf86cmap.h"
#include "shadowfb.h"

#include "xf86Cursor.h"
#include "xf86DDC.h"

#include "region.h"

#include <X11/extensions/randr.h>

#include <X11/extensions/dpmsconst.h>

#include "omap_driver.h"

#include "xf86Crtc.h"

#include "xf86drmMode.h"
#include "drm_fourcc.h"
#include "X11/Xatom.h"

#include <sys/ioctl.h>
#include <libudev.h>

typedef struct {
	/* hardware cursor: */
	uint32_t plane_id;
	struct omap_bo *bo;
	uint32_t fb_id;
	uint32_t zpos_prop_id;
	int x, y;
} drmmode_cursor_rec, *drmmode_cursor_ptr;

typedef struct {
	int fd;
	drmModeResPtr mode_res;
	int cpp;
	struct udev_monitor *uevent_monitor;
	InputHandlerProc uevent_handler;
	drmmode_cursor_ptr cursor;
} drmmode_rec, *drmmode_ptr;

typedef struct {
	drmmode_ptr drmmode;
	drmModeCrtcPtr mode_crtc;
	int cursor_visible;
} drmmode_crtc_private_rec, *drmmode_crtc_private_ptr;

typedef struct {
	drmModePropertyPtr mode_prop;
	int index; /* Index within the kernel-side property arrays for
	 * this connector. */
	int num_atoms; /* if range prop, num_atoms == 1; if enum prop,
	 * num_atoms == num_enums + 1 */
	Atom *atoms;
} drmmode_prop_rec, *drmmode_prop_ptr;

typedef struct {
	drmmode_ptr drmmode;
	int output_id;
	drmModeConnectorPtr mode_output;
	drmModeEncoderPtr mode_encoder;
	drmModePropertyBlobPtr edid_blob;
	int num_props;
	drmmode_prop_ptr props;
} drmmode_output_private_rec, *drmmode_output_private_ptr;

static void drmmode_output_dpms(xf86OutputPtr output, int mode);

static OMAPScanoutPtr
drmmode_scanout_from_size(OMAPScanoutPtr scanouts, int x, int y, int width,
		int height)
{
	int i;
	for (i = 0; i < MAX_SCANOUTS; i++) {
		if (scanouts[i].x == x && scanouts[i].y == y &&
		    scanouts[i].width == width && scanouts[i].height == height)
			return &scanouts[i];
	}
	return NULL;
}

static OMAPScanoutPtr
drmmode_scanout_from_crtc(OMAPScanoutPtr scanouts, xf86CrtcPtr crtc)
{
	return drmmode_scanout_from_size(scanouts, crtc->x, crtc->y,
			crtc->mode.HDisplay, crtc->mode.VDisplay);
}

OMAPScanoutPtr
drmmode_scanout_from_drawable(OMAPScanoutPtr scanouts, DrawablePtr pDraw)
{
	return drmmode_scanout_from_size(scanouts, pDraw->x, pDraw->y,
			pDraw->width, pDraw->height);
}

static OMAPScanoutPtr
drmmode_scanout_add(OMAPScanoutPtr scanouts, xf86CrtcPtr crtc,
		struct omap_bo *bo)
{
	int i;
	for (i = 0; i < MAX_SCANOUTS; i++) {
		OMAPScanoutPtr s = &scanouts[i];

		if (s->bo)
			continue;

		omap_bo_reference(bo);
		s->x = crtc->x;
		s->y = crtc->y;
		s->width = crtc->mode.HDisplay;
		s->height = crtc->mode.VDisplay;
		s->bo = bo;
		return s;
	}

	return NULL;
}

void
drmmode_scanout_set(OMAPScanoutPtr scanouts, int x, int y, struct omap_bo *bo)
{
	OMAPScanoutPtr s;

	s = drmmode_scanout_from_size(scanouts, x, y, omap_bo_width(bo),
			omap_bo_height(bo));
	if (!s) {
		/* The scanout may not exist after flip, just ignore */
		return;
	}

	omap_bo_reference(bo);
	omap_bo_unreference(s->bo);
	s->bo = bo;
}

int drmmode_crtc_id(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	return drmmode_crtc->mode_crtc->crtc_id;
}

int drmmode_crtc_index_from_drawable(ScrnInfoPtr pScrn, DrawablePtr pDraw)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86CrtcPtr crtc;
	DisplayModePtr mode;
	int i;

	for (i = 0; i < xf86_config->num_crtc; i++) {
		crtc = xf86_config->crtc[i];
		if (!crtc->enabled)
			continue;
		mode = &crtc->mode;
		if (crtc->x == pDraw->x && crtc->y == pDraw->y &&
		    mode->HDisplay == pDraw->width &&
		    mode->VDisplay == pDraw->height)
			return i;
	}
	return -1;
}

int drmmode_crtc_id_from_drawable(ScrnInfoPtr pScrn, DrawablePtr pDraw)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86CrtcPtr crtc;
	int index;

	index = drmmode_crtc_index_from_drawable(pScrn, pDraw);
	if (index == -1)
		return 0;
	crtc = xf86_config->crtc[index];
	return drmmode_crtc_id(crtc);
}

static drmmode_ptr
drmmode_from_scrn(ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	drmmode_crtc_private_ptr drmmode_crtc;

	drmmode_crtc = xf86_config->crtc[0]->driver_private;
	return drmmode_crtc->drmmode;
}

static void
drmmode_ConvertFromKMode(ScrnInfoPtr pScrn, drmModeModeInfo *kmode,
		DisplayModePtr	mode)
{
	memset(mode, 0, sizeof(DisplayModeRec));
	mode->status = MODE_OK;

	mode->Clock = kmode->clock;

	mode->HDisplay = kmode->hdisplay;
	mode->HSyncStart = kmode->hsync_start;
	mode->HSyncEnd = kmode->hsync_end;
	mode->HTotal = kmode->htotal;
	mode->HSkew = kmode->hskew;

	mode->VDisplay = kmode->vdisplay;
	mode->VSyncStart = kmode->vsync_start;
	mode->VSyncEnd = kmode->vsync_end;
	mode->VTotal = kmode->vtotal;
	mode->VScan = kmode->vscan;

	mode->Flags = kmode->flags; //& FLAG_BITS;
	mode->name = strdup(kmode->name);

	DEBUG_MSG("copy mode %s (%p %p)", kmode->name, mode->name, mode);

	if (kmode->type & DRM_MODE_TYPE_DRIVER)
		mode->type = M_T_DRIVER;
	if (kmode->type & DRM_MODE_TYPE_PREFERRED)
		mode->type |= M_T_PREFERRED;

	xf86SetModeCrtc (mode, pScrn->adjustFlags);
}

static void
drmmode_ConvertToKMode(ScrnInfoPtr pScrn, drmModeModeInfo *kmode,
		DisplayModePtr mode)
{
	memset(kmode, 0, sizeof(*kmode));

	kmode->clock = mode->Clock;
	kmode->hdisplay = mode->HDisplay;
	kmode->hsync_start = mode->HSyncStart;
	kmode->hsync_end = mode->HSyncEnd;
	kmode->htotal = mode->HTotal;
	kmode->hskew = mode->HSkew;

	kmode->vdisplay = mode->VDisplay;
	kmode->vsync_start = mode->VSyncStart;
	kmode->vsync_end = mode->VSyncEnd;
	kmode->vtotal = mode->VTotal;
	kmode->vscan = mode->VScan;

	kmode->flags = mode->Flags; //& FLAG_BITS;
	if (mode->name)
		strncpy(kmode->name, mode->name, DRM_DISPLAY_MODE_LEN);
	kmode->name[DRM_DISPLAY_MODE_LEN-1] = 0;
}

static void
drmmode_crtc_dpms(xf86CrtcPtr drmmode_crtc, int mode)
{
	// FIXME - Implement this function
}

static struct omap_bo *
drmmode_new_fb(OMAPPtr pOMAP, int width, int height, int depth, int bpp)
{
	return omap_bo_new_with_dim(pOMAP->dev, width, height, depth, bpp,
			OMAP_BO_SCANOUT | OMAP_BO_WC);
}

static int
drmmode_set_crtc_off(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	int ret;
	ret = drmModeSetCrtc(drmmode_crtc->drmmode->fd,
			drmmode_crtc->mode_crtc->crtc_id, 0, 0, 0, NULL, 0,
			NULL);
	return ret;
}

static int
drmmode_set_crtc(ScrnInfoPtr pScrn, xf86CrtcPtr crtc, struct omap_bo *bo, int x,
			int y)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output;
	drmmode_crtc_private_ptr drmmode_crtc;
	drmmode_output_private_ptr drmmode_output;
	int ret, crtc_id, output_count, i;
	uint32_t *output_ids = NULL;
	uint32_t fb_id;
	drmModeModeInfo kmode;

	fb_id = omap_bo_get_fb(bo);
	if (!fb_id) {
		ERROR_MSG("Cannot set CRTC without fb id");
		return -EFAULT;
	}
	output_ids = calloc(xf86_config->num_output, sizeof *output_ids);
	assert(output_ids);

	output_count = 0;
	for (i = 0; i < xf86_config->num_output; i++) {
		output = xf86_config->output[i];
		drmmode_output = output->driver_private;

		if (output->crtc != crtc)
			continue;

		output_ids[output_count]
			= drmmode_output->mode_output->connector_id;
		output_count++;
	}
	if (!output_count) {
		ERROR_MSG("No crtc outputs found");
		ret = -ENODEV;
		goto out;
	}

	drmmode_ConvertToKMode(pScrn, &kmode, &crtc->mode);

	drmmode_crtc = crtc->driver_private;
	crtc_id = drmmode_crtc->mode_crtc->crtc_id;
	ret = drmModeSetCrtc(drmmode_crtc->drmmode->fd, crtc_id,
			fb_id, x, y, output_ids, output_count,
			&kmode);
	if (ret) {
		ERROR_MSG("crtc_id:%d failed to set mode: %s",
			  crtc_id, strerror(-ret));
		goto out;
	}

out:
	free(output_ids);
	return ret;
}

/*
 * Copy region of @src starting at (@src_x, @src_y) to @dst at (@dst_x, dst_y).
 * This function does no conversions, so it assumes same src and dst bpp.
 *  @src         source buffer
 *  @src_x       x coordinate from which to start copying
 *  @src_y       y coordinate from which to start copying
 *  @src_width   max number of pixels per src row to copy
 *  @src_height  max number of src rows to copy
 *  @src_pitch   total length of each src row, in bytes
 *  @src_cpp     bytes (ie, chars) per pixel of source
 *  @dst         destination buffer
 *  @dst_x       x coordinate to which to start copying
 *  @dst_y       y coordinate to which to start copying
 *  @dst_width   max number of pixels per dst row to copy
 *  @dst_height  max number of dst rows to copy
 *  @dst_pitch   total length of each dst row, in bytes
 *  @dst_cpp     bytes (ie, chars) per pixel of dst, must be same as src_cpp
 */
static void
drmmode_copy_from_to(const uint8_t *src, int src_x, int src_y, int src_width,
		     int src_height, int src_pitch, int src_cpp,
		     uint8_t *dst, int dst_x, int dst_y, int dst_width,
		     int dst_height, int dst_pitch, int dst_cpp)
{
	int y;
	int src_x_start = max(dst_x - src_x, 0);
	int dst_x_start = max(src_x - dst_x, 0);
	int src_y_start = max(dst_y - src_y, 0);
	int dst_y_start = max(src_y - dst_y, 0);
	int width = min(src_width - src_x_start, dst_width - dst_x_start);
	int height = min(src_height - src_y_start, dst_height - dst_y_start);


	assert(src_cpp == dst_cpp);

	if (width <= 0 || height <= 0)
		return;

	src += src_y_start * src_pitch + src_x_start * src_cpp;
	dst += dst_y_start * dst_pitch + dst_x_start * src_cpp;

	for (y = 0; y < height; y++, src += src_pitch, dst += dst_pitch)
		memcpy(dst, src, width * dst_cpp);
}

/*
 * Copy region of src buffer located at (src_x, src_y) that overlaps the dst
 * buffer at dst_x, dst_y.
 * This function does no conversions, so it assumes same bpp and depth.
 * It also assumes the two regions are non-overlapping memory areas, even though
 * they may overlap in pixel space.
 */
static int
drmmode_copy_bo(ScrnInfoPtr pScrn, struct omap_bo *src_bo, int src_x, int src_y,
		struct omap_bo *dst_bo, int dst_x, int dst_y)
{
	void *dst;
	const void *src;

	if (!src_bo || !dst_bo) {
		ERROR_MSG("copy_bo received invalid arguments");
		return -EINVAL;
	}

	assert(omap_bo_bpp(src_bo) == omap_bo_bpp(dst_bo));

	src = omap_bo_map(src_bo);
	if (!src) {
		ERROR_MSG("Couldn't map src bo");
		return -EIO;
	}
	dst = omap_bo_map(dst_bo);
	if (!dst) {
		ERROR_MSG("Couldn't map dst bo");
		return -EIO;
	}

	// acquire for write first, so if (probably impossible) src==dst acquire
	// for read can succeed
	omap_bo_cpu_prep(dst_bo, OMAP_GEM_WRITE);
	omap_bo_cpu_prep(src_bo, OMAP_GEM_READ);

	drmmode_copy_from_to(src, src_x, src_y,
			     omap_bo_width(src_bo), omap_bo_height(src_bo),
			     omap_bo_pitch(src_bo), omap_bo_Bpp(src_bo),
			     dst, dst_x, dst_y,
			     omap_bo_width(dst_bo), omap_bo_height(dst_bo),
			     omap_bo_pitch(dst_bo), omap_bo_Bpp(dst_bo));

	omap_bo_cpu_fini(src_bo, 0);
	omap_bo_cpu_fini(dst_bo, 0);

	return 0;
}

static Bool drmmode_set_blit_crtc(ScrnInfoPtr pScrn, xf86CrtcPtr crtc)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	int rc;

	if (!crtc->enabled)
		return TRUE;

	/* Note: drmmode_set_crtc returns <0 on error, not a Bool */
	rc = drmmode_set_crtc(pScrn, crtc, pOMAP->scanout, crtc->x, crtc->y);
	if (rc) {
		ERROR_MSG("[CRTC:%u] set root scanout failed",
				drmmode_crtc_id(crtc));
		drmmode_set_crtc_off(crtc);
		return FALSE;
	}

	return TRUE;
}

static Bool drmmode_set_flip_crtc(ScrnInfoPtr pScrn, xf86CrtcPtr crtc)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	OMAPScanoutPtr scanout;
	int rc;

	if (!crtc->enabled)
		return TRUE;

	scanout = drmmode_scanout_from_crtc(pOMAP->scanouts, crtc);
	if (!scanout)
		return TRUE;

	/* Note: drmmode_set_crtc returns <0 on error, not a Bool */
	rc = drmmode_set_crtc(pScrn, crtc, scanout->bo, 0, 0);
	if (rc) {
		ERROR_MSG("[CRTC:%u] set per-crtc scanout failed",
				drmmode_crtc_id(crtc));
		drmmode_set_crtc_off(crtc);
		return FALSE;
	}

	return TRUE;
}

Bool drmmode_update_scanout_from_crtcs(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	int ret, i;
	Bool res;

	if (pOMAP->flip_mode == OMAP_FLIP_DISABLED)
		return TRUE;

	TRACE_ENTER();

	/* Only copy if source is valid. */
	for (i = 0; i < MAX_SCANOUTS; i++) {
		OMAPScanoutPtr scanout = &pOMAP->scanouts[i];

		if (!scanout->bo)
			continue;
		if (!scanout->valid)
			continue;

		ret = drmmode_copy_bo(pScrn, scanout->bo, scanout->x,
					  scanout->y, pOMAP->scanout, 0, 0);
		if (ret) {
			ERROR_MSG("Copy crtc to scanout failed");
			res = FALSE;
			goto out;
		}
	}
	res = TRUE;
out:
	TRACE_EXIT();
	return res;
}

/*
 * Enter blit mode.
 *
 * First, wait for all pending flips to complete.
 * Next, copy all valid per-crtc bo contents to the root bo, and mark their
 * scanouts as invalid to ensure they get updated when switching back to flip
 * mode.
 * Lastly, set all enabled crtcs to scan out from the root bo.
 */
Bool drmmode_set_blit_mode(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int ret, i;

	if (pOMAP->flip_mode == OMAP_FLIP_DISABLED)
		return TRUE;

	// wait for all flips to finish so we will read from the current buffer
	while (pOMAP->pending_flips > 0)
		drmmode_wait_for_event(pScrn);

	/* Only copy if source is valid. */
	for (i = 0; i < MAX_SCANOUTS; i++) {
		OMAPScanoutPtr scanout = &pOMAP->scanouts[i];

		if (!scanout->bo)
			continue;
		if (!scanout->valid)
			continue;

		ret = drmmode_copy_bo(pScrn, scanout->bo, scanout->x,
					  scanout->y, pOMAP->scanout, 0, 0);
		if (ret) {
			ERROR_MSG("Copy crtc to scanout failed");
			return FALSE;
		}
		scanout->valid = FALSE;
	}
	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		if (!drmmode_set_blit_crtc(pScrn, crtc)) {
			ERROR_MSG("[CRTC:%u] could not set blit mode",
					drmmode_crtc_id(crtc));
			goto unwind;
		}
	}
	pOMAP->flip_mode = OMAP_FLIP_DISABLED;
	return TRUE;

unwind:
	/* try restoring already transitioned CRTCs back to flip mode */
	while (--i >= 0) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		if (!drmmode_set_flip_crtc(pScrn, crtc))
			ERROR_MSG("[CRTC:%u] could not restore flip mode",
					drmmode_crtc_id(crtc));
	}
	return FALSE;
}

/*
 * Enter flip mode.
 *
 * First, copy contents from the root bo to each invalid per-crtc bo, and mark
 * its scanout as valid.
 * Lastly, set all enabled crtcs to scan out from their per-crtc bos.
 */
Bool drmmode_set_flip_mode(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int ret, i;

	if (pOMAP->flip_mode == OMAP_FLIP_ENABLED)
		return TRUE;

	/* Only copy if destination is invalid. */
	for (i = 0; i < MAX_SCANOUTS; i++) {
		OMAPScanoutPtr scanout = &pOMAP->scanouts[i];

		if (!scanout->bo)
			continue;
		if (scanout->valid)
			continue;

		ret = drmmode_copy_bo(pScrn, pOMAP->scanout, 0, 0,
					  scanout->bo, scanout->x,
					  scanout->y);
		if (ret) {
			ERROR_MSG("Copy scanout to crtc failed");
			return FALSE;
		}
		scanout->valid = TRUE;
	}

	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		if (!drmmode_set_flip_crtc(pScrn, crtc)) {
			ERROR_MSG("[CRTC:%u] could not set flip mode",
					drmmode_crtc_id(crtc));
			goto unwind;
		}
	}
	pOMAP->flip_mode = OMAP_FLIP_ENABLED;
	return TRUE;

unwind:
	/* try restoring already transitioned CRTCs back to blit mode */
	while (--i >= 0) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		if (!drmmode_set_blit_crtc(pScrn, crtc))
			ERROR_MSG("[CRTC:%u] could not restore blit mode",
					drmmode_crtc_id(crtc));
	}
	return FALSE;
}

static Bool drmmode_update_scanouts(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	OMAPScanoutPtr scanout;
	int i;
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86CrtcPtr crtc;
	struct omap_bo *bo;
	Bool valid;

	OMAPScanout old_scanouts[MAX_SCANOUTS];
	memcpy(old_scanouts, pOMAP->scanouts, sizeof(old_scanouts));
	memset(pOMAP->scanouts, 0, sizeof(pOMAP->scanouts));

	for (i = 0; i < xf86_config->num_crtc; i++) {
		crtc = xf86_config->crtc[i];
		if (!crtc->enabled || !crtc->mode.HDisplay ||
				!crtc->mode.VDisplay)
			continue;

		scanout = drmmode_scanout_from_crtc(old_scanouts, crtc);
		if (scanout) {
			/* Use existing BO */
			bo = scanout->bo;
			valid = scanout->valid;
			memset(scanout, 0, sizeof(*scanout));
		} else {
			/* Allocate a new BO */
			bo = drmmode_new_fb(pOMAP, crtc->mode.HDisplay,
				crtc->mode.VDisplay, pScrn->depth,
				pScrn->bitsPerPixel);
			if (!bo) {
				ERROR_MSG("Scanout buffer allocation failed");
				return FALSE;
			}
			valid = FALSE;
		}
		scanout = drmmode_scanout_add(pOMAP->scanouts, crtc, bo);
		if (!scanout) {
			ERROR_MSG("Add scanout failed");
			omap_bo_unreference(bo);
			return FALSE;
		}
		scanout->valid = valid;

		/*
		 * drmmode_scanout_add() adds a reference, but we either:
		 * * already have a reference, from a recycled BO
		 * * was given a reference when a new BO was allocated
		 */
		omap_bo_unreference(bo);
	}

	/* Drop the remaining unused BOs. */
	for (i = 0; i < MAX_SCANOUTS; i++)
		if (old_scanouts[i].bo != NULL) {
			/*
			 * Set has_resized when discarding active scanouts. This
			 * ensures we trigger the blit/flip logic which will
			 * setcrtc to a valid fb if needed
			 */
			pOMAP->has_resized = TRUE;
			omap_bo_unreference(old_scanouts[i].bo);
		}

	return TRUE;
}

static Bool
drmmode_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
		Rotation rotation, int x, int y)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	ScrnInfoPtr pScrn = crtc->scrn;
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
	int ret;
	int i;

	TRACE_ENTER();

	ret = xf86CrtcRotate(crtc);
	if (!ret)
		goto done;

	// On a modeset, we should switch to blit mode to get a single scanout buffer
	// and we will switch back to flip mode on the next flip request
	if (pOMAP->flip_mode == OMAP_FLIP_DISABLED)
		ret = drmmode_set_crtc(pScrn, crtc, pOMAP->scanout, crtc->x, crtc->y) == 0;
	else
		ret = drmmode_set_blit_mode(pScrn);
	if (!ret)
		goto done;

	// Fixme - Intel puts this function here, and Nouveau puts it at the end
	// of this function -> determine what's best for TI'S OMAP4:
	if (crtc->funcs->gamma_set)
		crtc->funcs->gamma_set(crtc, crtc->gamma_red, crtc->gamma_green,
				       crtc->gamma_blue, crtc->gamma_size);

	ret = drmmode_update_scanouts(pScrn);
	if (!ret) {
		ERROR_MSG("Update scanouts failed");
		goto done;
	}

	// FIXME - DO WE NEED TO CALL TO THE PVR EXA/DRI2 CODE TO UPDATE THEM???

	/* Turn on any outputs on this crtc that may have been disabled: */
	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];

		if (output->crtc != crtc)
			continue;

		drmmode_output_dpms(output, DPMSModeOn);
	}

	/*
	 * The screen has reconfigured, so reload hw cursor images as needed,
	 * and adjust cursor positions.
	 */
	if (drmmode->cursor)
		xf86_reload_cursors(pScrn->pScreen);

done:
	TRACE_EXIT();
	return ret;
}

/* Two different workarounds at play here:
 * * Mali has trouble with the cursor overlay when the visible portion has
 *   width less than the minimum FIMD DMA burst, i.e. 32 bytes (8 pixels
 *   ARGB).  This is a problem when the cursor is at the left or right edge
 *   of the screen.  Work around this padding the cursor on the left and
 *   right sides.
 * * X has trouble with cursor dimensions that aren't a multiple of 32,
 *   because it expects bitmask cursors to have a bit pitch the size of the
 *   typical machine word, i.e. 32 bits.
 * We create a 96x64 pixel cursor overlay, with 16 pixels' (64 bytes')
 * worth of padding on each side, so we can export a 64x64 size to X as we'd
 * like to have HW accelerated cursors at least to this size.  We stay with
 * multiple-of-32-pixel sizes internally for efficiency since the longest FIMD
 * DMA burst length is 128 bytes (32 pixels ARGB).
 */

#define CURSORW  96
#define CURSORH  64
#define CURSORPAD 16

static void
drmmode_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	drmmode_cursor_ptr cursor = drmmode->cursor;
	int crtc_x, crtc_y, src_x, src_y, w, h;

	if (!cursor)
		return;

	cursor->x = x;
	cursor->y = y;

	if (!drmmode_crtc->cursor_visible)
		return;

	w = CURSORW;
	h = CURSORH;
	crtc_x = cursor->x - CURSORPAD;
	crtc_y = cursor->y;
	src_x = 0;
	src_y = 0;

	if (crtc_x < 0) {
		src_x += -crtc_x;
		w -= -crtc_x;
		crtc_x = 0;
	}

	if (crtc_y < 0) {
		src_y += -crtc_y;
		h -= -crtc_y;
		crtc_y = 0;
	}

	if ((crtc_x + w) > crtc->mode.HDisplay) {
		w = crtc->mode.HDisplay - crtc_x;
	}

	if ((crtc_y + h) > crtc->mode.VDisplay) {
		h = crtc->mode.VDisplay - crtc_y;
	}

	/* note src coords (last 4 args) are in Q16 format */
	drmModeSetPlane(drmmode->fd, cursor->plane_id,
			drmmode_crtc->mode_crtc->crtc_id, cursor->fb_id, 0,
			crtc_x, crtc_y, w, h, src_x<<16, src_y<<16, w<<16, h<<16);
}

static void
drmmode_hide_cursor(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	drmmode_cursor_ptr cursor = drmmode->cursor;

	if (!cursor)
		return;

	drmmode_crtc->cursor_visible = FALSE;

	/* set plane's fb_id to 0 to disable it */
	drmModeSetPlane(drmmode->fd, cursor->plane_id,
			drmmode_crtc->mode_crtc->crtc_id, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0);
}

static void
drmmode_show_cursor(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	drmmode_cursor_ptr cursor = drmmode->cursor;

	if (!cursor)
		return;

	drmmode_crtc->cursor_visible = TRUE;

	drmModeObjectSetProperty(drmmode->fd, cursor->plane_id,
				 DRM_MODE_OBJECT_PLANE,
				 cursor->zpos_prop_id, 1);

	drmmode_set_cursor_position(crtc, cursor->x, cursor->y);
}

static void
drmmode_load_cursor_argb(xf86CrtcPtr crtc, CARD32 *image)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	drmmode_cursor_ptr cursor = drmmode->cursor;
	int row, visible;
	void* dst;
	const char* src_row;
	char* dst_row;

	if (!cursor)
		return;

	visible = drmmode_crtc->cursor_visible;

	if (visible)
		drmmode_hide_cursor(crtc);

	dst = omap_bo_map(cursor->bo);
	omap_bo_cpu_prep(cursor->bo, OMAP_GEM_WRITE);
	for (row = 0; row < CURSORH; row += 1) {
		// we're operating with ARGB data (32bpp)
		src_row = (const char*)image + row * 4 * (CURSORW - 2 * CURSORPAD);
		dst_row = (char*)dst + row * 4 * CURSORW;

		// copy CURSORPAD pad bytes, then data, then CURSORPAD more pad bytes
		memset(dst_row, 0, (4 * CURSORPAD));
		memcpy(dst_row + (4 * CURSORPAD), src_row, 4 * (CURSORW - 2 * CURSORPAD));
		memset(dst_row + 4 * (CURSORW - CURSORPAD), 0, (4 * CURSORPAD));
	}
	omap_bo_cpu_fini(cursor->bo, 0);

	if (visible)
		drmmode_show_cursor(crtc);
}

Bool
drmmode_cursor_init(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	drmmode_cursor_ptr cursor;
	drmModePlaneRes *plane_resources;
	drmModeObjectPropertiesPtr props;
	drmModePropertyPtr prop;
	int i;
	uint32_t plane_id;
	int zpos_prop_id;
	Bool ret = FALSE;

	/* technically we probably don't have any size limit.. since we
	 * are just using an overlay... but xserver will always create
	 * cursor images in the max size, so don't use width/height values
	 * that are too big
	 */
	const int w = CURSORW, h = CURSORH;
	uint32_t handles[4], pitches[4], offsets[4]; /* we only use [0] */

	TRACE_ENTER();

	if (drmmode->cursor) {
		INFO_MSG("HW Cursor already initialized");
		goto out;
	}

	/* find an unused plane which can be used as a mouse cursor.  Note
	 * that we cheat a bit, in order to not burn one overlay per crtc,
	 * and only show the mouse cursor on one crtc at a time
	 */
	plane_resources = drmModeGetPlaneResources(drmmode->fd);
	if (!plane_resources) {
		ERROR_MSG("drmModeGetPlaneResources failed: %s", strerror(errno));
		goto out;
	}

	if (plane_resources->count_planes < 1) {
		ERROR_MSG("HW Cursor: not enough planes");
		drmModeFreePlaneResources(plane_resources);
		goto out;
	}

	/* HACK: HW Cursor always uses the first plane */
	plane_id = plane_resources->planes[0];
	INFO_MSG("HW Cursor using [PLANE:%u]", plane_id);

	drmModeFreePlaneResources(plane_resources);

	props = drmModeObjectGetProperties(drmmode->fd, plane_id,
					   DRM_MODE_OBJECT_PLANE);
	if (!props) {
		ERROR_MSG("No properties found for DRM [PLANE:%u]", plane_id);
		goto out;
	}

	/* Find first "zpos" property for our HW Cursor plane */
	zpos_prop_id = -1;
	for (i = 0; i < props->count_props && zpos_prop_id == -1; i++) {
		prop = drmModeGetProperty(drmmode->fd, props->props[i]);

		if (!strcmp(prop->name, "zpos"))
			zpos_prop_id = prop->prop_id;
		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);

	if (zpos_prop_id == -1) {
		ERROR_MSG("No 'zpos' property found for [PLANE:%u]", plane_id);
		goto out;
	}

	cursor = calloc(1, sizeof *cursor);
	if (!cursor) {
		ERROR_MSG("HW Cursor allocation failed");
		goto out;
	}
	cursor->zpos_prop_id = zpos_prop_id;
	cursor->plane_id = plane_id;
	cursor->bo = omap_bo_new_with_dim(pOMAP->dev, w, h, 0, 32,
			OMAP_BO_SCANOUT | OMAP_BO_WC);
	if (!cursor->bo) {
		ERROR_MSG("error allocating hw cursor buffer");
		goto err_free_cursor;
	}

	handles[0] = omap_bo_handle(cursor->bo);
	pitches[0] = omap_bo_pitch(cursor->bo);
	offsets[0] = 0;

	if (drmModeAddFB2(drmmode->fd, w, h, DRM_FORMAT_ARGB8888,
			handles, pitches, offsets, &cursor->fb_id, 0)) {
		ERROR_MSG("drmModeAddFB2 failed: %s", strerror(errno));
		goto err_unref_cursor;
	}

	INFO_MSG("HW Cursor using [FB:%u]", cursor->fb_id);

	// see definition of CURSORPAD
	if (!xf86_cursors_init(pScreen, w - 2 * CURSORPAD, h,
			HARDWARE_CURSOR_ARGB |
			HARDWARE_CURSOR_UPDATE_UNHIDDEN)) {
		ERROR_MSG("xf86_cursors_init() failed");
		goto err_rm_fb;
	}

	INFO_MSG("HW cursor initialized");
	drmmode->cursor = cursor;

	ret = TRUE;
	goto out;

err_rm_fb:
	if (drmModeRmFB(drmmode->fd, cursor->fb_id))
		ERROR_MSG("drmModeRmFB(%u) failed: %s", cursor->fb_id,
				strerror(errno));
err_unref_cursor:
	omap_bo_unreference(cursor->bo);
err_free_cursor:
	free(cursor);
out:
	TRACE_EXIT();
	return ret;
}

void
drmmode_cursor_fini(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	drmmode_cursor_ptr cursor = drmmode->cursor;

	if (!cursor)
		return;

	xf86_cursors_fini(pScreen);

	/* Report any errors, but keep going... */
	if (drmModeRmFB(drmmode->fd, cursor->fb_id))
		ERROR_MSG("drmModeRmFB(%u) failed: %s", cursor->fb_id,
				strerror(errno));

	omap_bo_unreference(cursor->bo);

	free(cursor);
	drmmode->cursor = NULL;
}

#ifdef OMAP_SUPPORT_GAMMA
static void
drmmode_gamma_set(xf86CrtcPtr crtc, CARD16 *red, CARD16 *green, CARD16 *blue,
		int size)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	int ret;

	ret = drmModeCrtcSetGamma(drmmode->fd, drmmode_crtc->mode_crtc->crtc_id,
			size, red, green, blue);
	if (ret != 0) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				"failed to set gamma: %s\n", strerror(-ret));
	}
}
#endif

static const xf86CrtcFuncsRec drmmode_crtc_funcs = {
		.dpms = drmmode_crtc_dpms,
		.set_mode_major = drmmode_set_mode_major,
		.set_cursor_position = drmmode_set_cursor_position,
		.show_cursor = drmmode_show_cursor,
		.hide_cursor = drmmode_hide_cursor,
		.load_cursor_argb = drmmode_load_cursor_argb,
#ifdef OMAP_SUPPORT_GAMMA
		.gamma_set = drmmode_gamma_set,
#endif
};


static void
drmmode_crtc_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, int num)
{
	xf86CrtcPtr crtc;
	drmmode_crtc_private_ptr drmmode_crtc;

	TRACE_ENTER();

	crtc = xf86CrtcCreate(pScrn, &drmmode_crtc_funcs);
	if (crtc == NULL)
		return;

	drmmode_crtc = xnfcalloc(1, sizeof *drmmode_crtc);
	drmmode_crtc->mode_crtc = drmModeGetCrtc(drmmode->fd,
			drmmode->mode_res->crtcs[num]);
	drmmode_crtc->drmmode = drmmode;

	// FIXME - potentially add code to allocate a HW cursor here.

	crtc->driver_private = drmmode_crtc;

	TRACE_EXIT();
	return;
}

static xf86OutputStatus
drmmode_output_detect(xf86OutputPtr output)
{
	/* go to the hw and retrieve a new output struct */
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	xf86OutputStatus status;
	drmModeFreeConnector(drmmode_output->mode_output);

	drmmode_output->mode_output =
			drmModeGetConnector(drmmode->fd, drmmode_output->output_id);

	switch (drmmode_output->mode_output->connection) {
	case DRM_MODE_CONNECTED:
		status = XF86OutputStatusConnected;
		break;
	case DRM_MODE_DISCONNECTED:
		status = XF86OutputStatusDisconnected;
		break;
	default:
	case DRM_MODE_UNKNOWNCONNECTION:
		status = XF86OutputStatusUnknown;
		break;
	}
	return status;
}

static Bool
drmmode_output_mode_valid(xf86OutputPtr output, DisplayModePtr mode)
{
	if (mode->type & M_T_DEFAULT)
		/* Default modes are harmful here. */
		return MODE_BAD;

	return MODE_OK;
}

static DisplayModePtr
drmmode_output_get_modes(xf86OutputPtr output)
{
	ScrnInfoPtr pScrn = output->scrn;
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmModeConnectorPtr koutput = drmmode_output->mode_output;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	DisplayModePtr Modes = NULL, Mode;
	drmModePropertyPtr prop;
	xf86MonPtr ddc_mon = NULL;
	int i;

	/* look for an EDID property */
	for (i = 0; i < koutput->count_props; i++) {
		prop = drmModeGetProperty(drmmode->fd, koutput->props[i]);
		if (!prop)
			continue;

		if ((prop->flags & DRM_MODE_PROP_BLOB) &&
		    !strcmp(prop->name, "EDID")) {
			if (drmmode_output->edid_blob)
				drmModeFreePropertyBlob(drmmode_output->edid_blob);
			drmmode_output->edid_blob =
					drmModeGetPropertyBlob(drmmode->fd,
							koutput->prop_values[i]);
		}
		drmModeFreeProperty(prop);
	}

	if (drmmode_output->edid_blob)
		ddc_mon = xf86InterpretEDID(pScrn->scrnIndex,
				drmmode_output->edid_blob->data);

	if (ddc_mon) {
		xf86OutputSetEDID(output, ddc_mon);
		xf86SetDDCproperties(pScrn, ddc_mon);
	}

	DEBUG_MSG("count_modes: %d", koutput->count_modes);

	/* modes should already be available */
	for (i = 0; i < koutput->count_modes; i++) {
		Mode = xnfalloc(sizeof(DisplayModeRec));

		drmmode_ConvertFromKMode(pScrn, &koutput->modes[i],
				Mode);
		Modes = xf86ModesAdd(Modes, Mode);

	}
	return Modes;
}

static void
drmmode_output_destroy(xf86OutputPtr output)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	int i;

	if (drmmode_output->edid_blob)
		drmModeFreePropertyBlob(drmmode_output->edid_blob);
	for (i = 0; i < drmmode_output->num_props; i++) {
		drmModeFreeProperty(drmmode_output->props[i].mode_prop);
		free(drmmode_output->props[i].atoms);
	}
	free(drmmode_output->props);
	drmModeFreeConnector(drmmode_output->mode_output);
	free(drmmode_output);
	output->driver_private = NULL;
}

static void
drmmode_output_dpms(xf86OutputPtr output, int mode)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmModeConnectorPtr koutput = drmmode_output->mode_output;
	drmModePropertyPtr prop;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	int mode_id = -1, i;

	for (i = 0; i < koutput->count_props; i++) {
		prop = drmModeGetProperty(drmmode->fd, koutput->props[i]);
		if (!prop)
			continue;
		if ((prop->flags & DRM_MODE_PROP_ENUM) &&
		    !strcmp(prop->name, "DPMS")) {
			mode_id = koutput->props[i];
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}

	if (mode_id < 0)
		return;

	drmModeConnectorSetProperty(drmmode->fd, koutput->connector_id,
			mode_id, mode);
}

static Bool
drmmode_property_ignore(drmModePropertyPtr prop)
{
	if (!prop)
		return TRUE;
	/* ignore blob prop */
	if (prop->flags & DRM_MODE_PROP_BLOB)
		return TRUE;
	/* ignore standard property */
	if (!strcmp(prop->name, "EDID") ||
			!strcmp(prop->name, "DPMS"))
		return TRUE;

	return FALSE;
}

static void
drmmode_output_create_resources(xf86OutputPtr output)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmModeConnectorPtr mode_output = drmmode_output->mode_output;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	drmModePropertyPtr drmmode_prop;
	uint32_t value;
	int i, j, err;

	drmmode_output->props = calloc(mode_output->count_props,
			sizeof *drmmode_output->props);
	if (!drmmode_output->props)
		return;

	drmmode_output->num_props = 0;
	for (i = 0, j = 0; i < mode_output->count_props; i++) {
		drmmode_prop = drmModeGetProperty(drmmode->fd, mode_output->props[i]);
		if (drmmode_property_ignore(drmmode_prop)) {
			drmModeFreeProperty(drmmode_prop);
			continue;
		}
		drmmode_output->props[j].mode_prop = drmmode_prop;
		drmmode_output->props[j].index = i;
		drmmode_output->num_props++;
		j++;
	}

	for (i = 0; i < drmmode_output->num_props; i++) {
		drmmode_prop_ptr p = &drmmode_output->props[i];
		drmmode_prop = p->mode_prop;

		value = drmmode_output->mode_output->prop_values[p->index];

		if (drmmode_prop->flags & DRM_MODE_PROP_RANGE) {
			INT32 range[2];

			p->num_atoms = 1;
			p->atoms = calloc(p->num_atoms, sizeof *p->atoms);
			if (!p->atoms)
				continue;
			p->atoms[0] = MakeAtom(drmmode_prop->name, strlen(drmmode_prop->name), TRUE);
			range[0] = drmmode_prop->values[0];
			range[1] = drmmode_prop->values[1];
			err = RRConfigureOutputProperty(output->randr_output, p->atoms[0],
					FALSE, TRUE,
					drmmode_prop->flags & DRM_MODE_PROP_IMMUTABLE ? TRUE : FALSE,
							2, range);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRConfigureOutputProperty error, %d\n", err);
			}
			err = RRChangeOutputProperty(output->randr_output, p->atoms[0],
					XA_INTEGER, 32, PropModeReplace, 1,
					&value, FALSE, FALSE);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRChangeOutputProperty error, %d\n", err);
			}
		} else if (drmmode_prop->flags & DRM_MODE_PROP_ENUM) {
			p->num_atoms = drmmode_prop->count_enums + 1;
			p->atoms = calloc(p->num_atoms, sizeof *p->atoms);
			if (!p->atoms)
				continue;
			p->atoms[0] = MakeAtom(drmmode_prop->name, strlen(drmmode_prop->name), TRUE);
			for (j = 1; j <= drmmode_prop->count_enums; j++) {
				struct drm_mode_property_enum *e = &drmmode_prop->enums[j-1];
				p->atoms[j] = MakeAtom(e->name, strlen(e->name), TRUE);
			}
			err = RRConfigureOutputProperty(output->randr_output, p->atoms[0],
					FALSE, FALSE,
					drmmode_prop->flags & DRM_MODE_PROP_IMMUTABLE ? TRUE : FALSE,
							p->num_atoms - 1, (INT32 *)&p->atoms[1]);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRConfigureOutputProperty error, %d\n", err);
			}
			for (j = 0; j < drmmode_prop->count_enums; j++)
				if (drmmode_prop->enums[j].value == value)
					break;
			/* there's always a matching value */
			err = RRChangeOutputProperty(output->randr_output, p->atoms[0],
					XA_ATOM, 32, PropModeReplace, 1, &p->atoms[j+1], FALSE, FALSE);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRChangeOutputProperty error, %d\n", err);
			}
		}
	}
}

static Bool
drmmode_output_set_property(xf86OutputPtr output, Atom property,
		RRPropertyValuePtr value)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	int i, ret;

	for (i = 0; i < drmmode_output->num_props; i++) {
		drmmode_prop_ptr p = &drmmode_output->props[i];

		if (p->atoms[0] != property)
			continue;

		if (p->mode_prop->flags & DRM_MODE_PROP_RANGE) {
			uint32_t val;

			if (value->type != XA_INTEGER || value->format != 32 ||
					value->size != 1)
				return FALSE;
			val = *(uint32_t *)value->data;

			ret = drmModeConnectorSetProperty(drmmode->fd, drmmode_output->output_id,
					p->mode_prop->prop_id, (uint64_t)val);

			if (ret)
				return FALSE;

			return TRUE;

		} else if (p->mode_prop->flags & DRM_MODE_PROP_ENUM) {
			Atom	atom;
			const char	*name;
			int		j;

			if (value->type != XA_ATOM || value->format != 32 || value->size != 1)
				return FALSE;
			memcpy(&atom, value->data, 4);
			name = NameForAtom(atom);
			if (name == NULL)
				return FALSE;

			/* search for matching name string, then set its value down */
			for (j = 0; j < p->mode_prop->count_enums; j++) {
				if (!strcmp(p->mode_prop->enums[j].name, name)) {
					ret = drmModeConnectorSetProperty(drmmode->fd,
							drmmode_output->output_id,
							p->mode_prop->prop_id,
							p->mode_prop->enums[j].value);

					if (ret)
						return FALSE;

					return TRUE;
				}
			}

			return FALSE;
		}
	}

	return TRUE;
}

static Bool
drmmode_output_get_property(xf86OutputPtr output, Atom property)
{

	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	uint32_t value;
	int err, i;

	if (output->scrn->vtSema) {
		drmModeFreeConnector(drmmode_output->mode_output);
		drmmode_output->mode_output =
				drmModeGetConnector(drmmode->fd, drmmode_output->output_id);
	}

	for (i = 0; i < drmmode_output->num_props; i++) {
		drmmode_prop_ptr p = &drmmode_output->props[i];
		if (p->atoms[0] != property)
			continue;

		value = drmmode_output->mode_output->prop_values[p->index];

		if (p->mode_prop->flags & DRM_MODE_PROP_RANGE) {
			err = RRChangeOutputProperty(output->randr_output,
					property, XA_INTEGER, 32,
					PropModeReplace, 1, &value,
					FALSE, FALSE);

			return !err;
		} else if (p->mode_prop->flags & DRM_MODE_PROP_ENUM) {
			int		j;

			/* search for matching name string, then set its value down */
			for (j = 0; j < p->mode_prop->count_enums; j++) {
				if (p->mode_prop->enums[j].value == value)
					break;
			}

			err = RRChangeOutputProperty(output->randr_output, property,
					XA_ATOM, 32, PropModeReplace, 1,
					&p->atoms[j+1], FALSE, FALSE);

			return !err;
		}
	}

	return FALSE;
}

static const xf86OutputFuncsRec drmmode_output_funcs = {
		.create_resources = drmmode_output_create_resources,
		.dpms = drmmode_output_dpms,
		.detect = drmmode_output_detect,
		.mode_valid = drmmode_output_mode_valid,
		.get_modes = drmmode_output_get_modes,
		.set_property = drmmode_output_set_property,
		.get_property = drmmode_output_get_property,
		.destroy = drmmode_output_destroy
};

// FIXME - Eliminate the following values that aren't accurate for OMAP4:
const char *output_names[] = { "None",
		"VGA",
		"DVI-I",
		"DVI-D",
		"DVI-A",
		"Composite",
		"SVIDEO",
		"LVDS",
		"CTV",
		"DIN",
		"DP",
		"HDMI",
		"HDMI",
		"TV",
		"eDP",
};
#define NUM_OUTPUT_NAMES (sizeof(output_names) / sizeof(output_names[0]))

static void
drmmode_output_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode, int num)
{
	xf86OutputPtr output;
	drmModeConnectorPtr koutput;
	drmModeEncoderPtr kencoder;
	drmmode_output_private_ptr drmmode_output;
	char name[32];

	TRACE_ENTER();

	koutput = drmModeGetConnector(drmmode->fd,
			drmmode->mode_res->connectors[num]);
	if (!koutput)
		return;

	kencoder = drmModeGetEncoder(drmmode->fd, koutput->encoders[0]);
	if (!kencoder) {
		drmModeFreeConnector(koutput);
		return;
	}

	if (koutput->connector_type >= NUM_OUTPUT_NAMES)
		snprintf(name, 32, "Unknown%d-%d", koutput->connector_type,
				koutput->connector_type_id);
	else
		snprintf(name, 32, "%s-%d",
				output_names[koutput->connector_type],
				koutput->connector_type_id);

	output = xf86OutputCreate(pScrn, &drmmode_output_funcs, name);
	if (!output) {
		drmModeFreeEncoder(kencoder);
		drmModeFreeConnector(koutput);
		return;
	}

	drmmode_output = calloc(1, sizeof *drmmode_output);
	if (!drmmode_output) {
		xf86OutputDestroy(output);
		drmModeFreeConnector(koutput);
		drmModeFreeEncoder(kencoder);
		return;
	}

	drmmode_output->output_id = drmmode->mode_res->connectors[num];
	drmmode_output->mode_output = koutput;
	drmmode_output->mode_encoder = kencoder;
	drmmode_output->drmmode = drmmode;

	output->mm_width = koutput->mmWidth;
	output->mm_height = koutput->mmHeight;
	output->driver_private = drmmode_output;

	output->possible_crtcs = kencoder->possible_crtcs;
	output->possible_clones = kencoder->possible_clones;
	output->interlaceAllowed = TRUE;

	TRACE_EXIT();
	return;
}

static Bool
drmmode_xf86crtc_resize(ScrnInfoPtr pScrn, int width, int height)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	ScreenPtr pScreen = pScrn->pScreen;
	struct omap_bo *new_scanout;
	uint32_t pitch;

	TRACE_ENTER();

	/* if fb required size has changed, realloc! */

	DEBUG_MSG("Resize!  %dx%d", width, height);

	if (  (width != omap_bo_width(pOMAP->scanout))
	      || (height != omap_bo_height(pOMAP->scanout))
	      || (pScrn->bitsPerPixel != omap_bo_bpp(pOMAP->scanout)) ) {

		DEBUG_MSG("allocating new scanout buffer: %dx%d",
				width, height);

		/* allocate new scanout buffer */
		new_scanout = drmmode_new_fb(pOMAP, width,
					     height, pScrn->depth,
					     pScrn->bitsPerPixel);
		if (!new_scanout) {
			ERROR_MSG("Error reallocating scanout buffer");
			return FALSE;
		}

		pOMAP->has_resized = TRUE;
		omap_bo_unreference(pOMAP->scanout);
		pOMAP->scanout = new_scanout;
	}

	pScrn->virtualX = width;
	pScrn->virtualY = height;
	pitch = omap_bo_pitch(pOMAP->scanout);
	pScrn->displayWidth = pitch / ((pScrn->bitsPerPixel + 7) / 8);

	if (pScreen && pScreen->ModifyPixmapHeader) {
		PixmapPtr rootPixmap = pScreen->GetScreenPixmap(pScreen);
		pScreen->ModifyPixmapHeader(rootPixmap,
				pScrn->virtualX, pScrn->virtualY,
				pScrn->depth, pScrn->bitsPerPixel, pitch,
				omap_bo_map(pOMAP->scanout));
	}

	TRACE_EXIT();
	return TRUE;
}

static const xf86CrtcConfigFuncsRec drmmode_xf86crtc_config_funcs = {
		.resize = drmmode_xf86crtc_resize
};


Bool drmmode_pre_init(ScrnInfoPtr pScrn, int fd, int cpp)
{
	drmmode_ptr drmmode;
	int i;

	TRACE_ENTER();

	pScrn->canDoBGNoneRoot = TRUE;

	drmmode = calloc(1, sizeof *drmmode);
	drmmode->fd = fd;

	xf86CrtcConfigInit(pScrn, &drmmode_xf86crtc_config_funcs);


	drmmode->cpp = cpp;
	drmmode->mode_res = drmModeGetResources(drmmode->fd);
	if (!drmmode->mode_res) {
		return FALSE;
	} else {
		DEBUG_MSG("Got KMS resources");
		DEBUG_MSG("  %d connectors, %d encoders",
				drmmode->mode_res->count_connectors,
				drmmode->mode_res->count_encoders);
		DEBUG_MSG("  %d crtcs, %d fbs",
				drmmode->mode_res->count_crtcs, drmmode->mode_res->count_fbs);
		DEBUG_MSG("  %dx%d minimum resolution",
				drmmode->mode_res->min_width, drmmode->mode_res->min_height);
		DEBUG_MSG("  %dx%d maximum resolution",
				drmmode->mode_res->max_width, drmmode->mode_res->max_height);
	}
	xf86CrtcSetSizeRange(pScrn, 320, 200, drmmode->mode_res->max_width,
			drmmode->mode_res->max_height);
	for (i = 0; i < drmmode->mode_res->count_crtcs; i++)
		drmmode_crtc_init(pScrn, drmmode, i);

	for (i = 0; i < drmmode->mode_res->count_connectors; i++)
		drmmode_output_init(pScrn, drmmode, i);

	xf86InitialConfiguration(pScrn, TRUE);

	TRACE_EXIT();

	return TRUE;
}

void
drmmode_adjust_frame(ScrnInfoPtr pScrn, int x, int y)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output = config->output[config->compat_output];
	xf86CrtcPtr crtc = output->crtc;
	int saved_x, saved_y;
	Bool ret;

	if (!crtc || !crtc->enabled)
		return;

	saved_x = crtc->x;
	saved_y = crtc->y;
	ret = drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation, x, y);
	if (!ret) {
		crtc->x = saved_x;
		crtc->y = saved_y;
	}
}

/*
 * Page Flipping
 */

static void
page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec,
		unsigned int tv_usec, void *user_data)
{
	OMAPDRI2SwapComplete(user_data);
}

static drmEventContext event_context = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = page_flip_handler,
};

int
drmmode_page_flip(DrawablePtr draw, uint32_t fb_id, void *priv,
		int* num_flipped)
{
	ScreenPtr pScreen = draw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int ret, i;
	unsigned int flags = 0;

#if OMAP_USE_PAGE_FLIP_EVENTS
	flags |= DRM_MODE_PAGE_FLIP_EVENT;
#endif

	/* Flip all crtc's that match this drawable's position and size */
	*num_flipped = 0;
	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		uint32_t crtc_id = drmmode_crtc_id(crtc);
		Bool connected = FALSE;
		int j;

		if (!crtc->enabled)
			continue;
		/* crtc can be enabled but all the outputs disabled, which
		   will cause flip to fail with EBUSY, so don't even try.
		   eventually the mode on this CRTC will be disabled */
		for (j = 0; j < xf86_config->num_output; j++) {
			xf86OutputPtr output = xf86_config->output[j];
			connected = connected || (output->crtc == crtc
					&& output->status
					== XF86OutputStatusConnected);
		}
		if (!connected)
			continue;

		if (crtc->x != draw->x || crtc->y != draw->y ||
		    crtc->mode.HDisplay != draw->width ||
		    crtc->mode.VDisplay != draw->height)
			continue;

		DEBUG_MSG("[CRTC:%u] [FB:%u]", crtc_id, fb_id);
		ret = drmModePageFlip(pOMAP->drmFD, crtc_id, fb_id, flags,
				priv);
		if (ret) {
			ERROR_MSG("[CRTC:%u] [FB:%u] page flip failed: %s",
					crtc_id, fb_id, strerror(errno));
			return ret;
		}
		(*num_flipped)++;
	}
	return 0;
}

/*
 * Hot Plug Event handling:
 */

static void
drmmode_handle_uevents(int fd, void *closure)
{
	ScrnInfoPtr pScrn = closure;
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	struct udev_device *dev;
	const char *hotplug;
	struct stat s;
	dev_t udev_devnum;

	dev = udev_monitor_receive_device(drmmode->uevent_monitor);
	if (!dev)
		return;

	// FIXME - Do we need to keep this code, which Rob originally wrote
	// (i.e. up thru the "if" statement)?:

	/*
	 * Check to make sure this event is directed at our
	 * device (by comparing dev_t values), then make
	 * sure it's a hotplug event (HOTPLUG=1)
	 */
	udev_devnum = udev_device_get_devnum(dev);
	if (fstat(pOMAP->drmFD, &s)) {
		ERROR_MSG("fstat failed: %s", strerror(errno));
		udev_device_unref(dev);
		return;
	}

	hotplug = udev_device_get_property_value(dev, "HOTPLUG");

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "hotplug=%s, match=%d\n", hotplug,
			!memcmp(&s.st_rdev, &udev_devnum, sizeof (dev_t)));

	if (memcmp(&s.st_rdev, &udev_devnum, sizeof (dev_t)) == 0 &&
			hotplug && atoi(hotplug) == 1) {
		RRGetInfo(xf86ScrnToScreen(pScrn), TRUE);
	}
	udev_device_unref(dev);
}

static void
drmmode_uevent_init(ScrnInfoPtr pScrn)
{
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	struct udev *u;
	struct udev_monitor *mon;

	TRACE_ENTER();

	u = udev_new();
	if (!u)
		return;
	mon = udev_monitor_new_from_netlink(u, "udev");
	if (!mon) {
		udev_unref(u);
		return;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(mon,
			"drm",
			"drm_minor") < 0 ||
			udev_monitor_enable_receiving(mon) < 0) {
		udev_monitor_unref(mon);
		udev_unref(u);
		return;
	}

	drmmode->uevent_handler =
			xf86AddGeneralHandler(udev_monitor_get_fd(mon),
					drmmode_handle_uevents, pScrn);

	drmmode->uevent_monitor = mon;

	TRACE_EXIT();
}

static void
drmmode_uevent_fini(ScrnInfoPtr pScrn)
{
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);

	TRACE_ENTER();

	if (drmmode->uevent_handler) {
		struct udev *u = udev_monitor_get_udev(drmmode->uevent_monitor);
		xf86RemoveGeneralHandler(drmmode->uevent_handler);

		udev_monitor_unref(drmmode->uevent_monitor);
		udev_unref(u);
	}

	TRACE_EXIT();
}

static void
drmmode_wakeup_handler(pointer data, int err, pointer p)
{
	ScrnInfoPtr pScrn = data;
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	fd_set *read_mask = p;

	if (pScrn == NULL || err < 0)
		return;

	if (FD_ISSET(drmmode->fd, read_mask))
		drmHandleEvent(drmmode->fd, &event_context);
}

void
drmmode_wait_for_event(ScrnInfoPtr pScrn)
{
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	drmHandleEvent(drmmode->fd, &event_context);
}

void
drmmode_screen_init(ScrnInfoPtr pScrn)
{
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);

	drmmode_uevent_init(pScrn);

	AddGeneralSocket(drmmode->fd);

	/* Register a wakeup handler to get informed on DRM events */
	RegisterBlockAndWakeupHandlers((BlockHandlerProcPtr)NoopDDA,
			drmmode_wakeup_handler, pScrn);
}

void
drmmode_screen_fini(ScrnInfoPtr pScrn)
{
	drmmode_uevent_fini(pScrn);
}

void drmmode_copy_fb(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	int dst_cpp = (pScrn->bitsPerPixel + 7) / 8;
	uint32_t dst_pitch = pScrn->displayWidth * dst_cpp;
	int src_cpp;
	uint32_t src_pitch;
	unsigned int src_size;
	unsigned char *dst, *src;
	struct fb_var_screeninfo vinfo;
	int fd;
	int ret;

	if (!(dst = omap_bo_map(pOMAP->scanout))) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Couldn't map scanout bo\n");
		return;
	}

	fd = open("/dev/fb0", O_RDONLY | O_SYNC);
	if (fd == -1) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Couldn't open /dev/fb0: %s\n",
				strerror(errno));
		return;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Vscreeninfo ioctl failed: %s\n",
				strerror(errno));
		goto close_fd;
	}

	if (vinfo.bits_per_pixel != 32)
	{
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"FB found but not 32 bpp\n");
		goto close_fd;
	}

	src_cpp = (vinfo.bits_per_pixel + 7) / 8;
	src_pitch = vinfo.xres_virtual * src_cpp;
	src_size = vinfo.yres_virtual * src_pitch;

	src = mmap(NULL, src_size, PROT_READ, MAP_SHARED, fd, 0);
	if (src == MAP_FAILED) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Couldn't mmap /dev/fb0: %s\n",
				strerror(errno));
		goto close_fd;
	}

	ret = omap_bo_cpu_prep(pOMAP->scanout, OMAP_GEM_WRITE);
	if (ret)
		goto munmap_src;

	drmmode_copy_from_to(src, 0, 0, vinfo.xres_virtual, vinfo.yres_virtual,
			src_pitch, src_cpp,
			dst, 0, 0, pScrn->virtualX, pScrn->virtualY,
			dst_pitch, dst_cpp);

	omap_bo_cpu_fini(pOMAP->scanout, 0);

munmap_src:
	ret = munmap(src, src_size);
	if (ret == -1)
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Couldn't munmap /dev/fb0: %s\n",
				strerror(errno));

close_fd:
	close(fd);
}
