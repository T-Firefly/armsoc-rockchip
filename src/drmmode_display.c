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

#include "xorgVersion.h"



#include <sys/stat.h>




// START: THE FOLLOWING #include STATEMENTS WERE COPIED FROM "nv_include.h";
  // FIXME - DETERMINE WHICH OF THESE ARE REALLY/STILL NEEDED FOR THE TI DRIVER:
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

/* All drivers should typically include these */
#include "xf86.h"
#include "xf86_OSproc.h"
#define PPC_MMIO_IS_BE
#include "compiler.h"

/* Drivers for PCI hardware need this */
#include "xf86PciInfo.h"

/* Drivers that need to access the PCI config space directly need this */
#include "xf86Pci.h"

/* All drivers initialising the SW cursor need this */
#include "mipointer.h"

/* All drivers implementing backing store need this */
#include "mibstore.h"

#include "micmap.h"

#include "xf86DDC.h"

#include "vbe.h"

#include "xf86RandR12.h"

//#include "nv_const.h"

#include "dixstruct.h"
#include "scrnintstr.h"

#include "fb.h"

#include "xf86cmap.h"
#include "shadowfb.h"

#include "xf86xv.h"
#include <X11/extensions/Xv.h>

#include "xf86Cursor.h"
#include "xf86DDC.h"

#include "region.h"

#include <X11/extensions/randr.h>

#ifdef HAVE_XEXTPROTO_71
#include <X11/extensions/dpmsconst.h>
#else
#define DPMS_SERVER
#include <X11/extensions/dpms.h>
#endif

// END: THE PREVIOUS #include STATEMENTS WERE COPIED FROM "nv_include.h";




#include "omap_driver.h"

// START: SOME OTHER #include STATEMENTS THAT I NEEDED TO COMPILE.
  // FIXME - DETERMINE WHICH OF THESE ARE REALLY/STILL NEEDED, AND HOW TO
  // COALESCE THEM WITH THE OTHER #include STATEMENTS:
#include "xf86Crtc.h"
// END: SOME OTHER #include STATEMENTS THAT I NEEDED TO COMPILE.




#include "xf86drmMode.h"
#include "X11/Xatom.h"

#include <sys/ioctl.h>
#include <libudev.h>

typedef struct {
    int fd;
    uint32_t fb_id;
    drmModeResPtr mode_res;
    int cpp;
    struct udev_monitor *uevent_monitor;
    InputHandlerProc uevent_handler;
} drmmode_rec, *drmmode_ptr;

typedef struct {
    drmmode_ptr drmmode;
    drmModeCrtcPtr mode_crtc;
#if 0
    // Fixme - We need different code to deal with a cursor on OMAP4:
    struct nouveau_bo *cursor;
    struct nouveau_bo *rotate_bo;
#endif
    uint32_t rotate_pitch;
    PixmapPtr rotate_pixmap;
    uint32_t rotate_fb_id;
    Bool cursor_visible;
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
void drmmode_remove_fb(ScrnInfoPtr pScrn);

#if 0
static PixmapPtr
drmmode_pixmap_wrap(ScreenPtr pScreen, int width, int height, int depth,
		    int bpp, int pitch, struct nouveau_bo *bo)
{
	PixmapPtr ppix;

	ppix = pScreen->CreatePixmap(pScreen, 0, 0, depth, 0);
	if (!ppix)
		return NULL;

	pScreen->ModifyPixmapHeader(ppix, width, height, depth, bpp,
				    pitch, NULL);
	nouveau_bo_ref(bo, &nouveau_pixmap(ppix)->bo);

	return ppix;
}
#endif

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
} /* drmmode_ConvertFromKMode() */

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
} /* drmmode_ConvertToKMode() */

static void
drmmode_crtc_dpms(xf86CrtcPtr drmmode_crtc, int mode)
{
    // FIXME - Implement this function
} /* drmmode_crtc_dpms() */

static Bool
drmmode_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
                       Rotation rotation, int x, int y)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	int saved_x, saved_y;
	Rotation saved_rotation;
	DisplayModeRec saved_mode;
	uint32_t *output_ids = NULL;
	int output_count = 0;
	int ret = TRUE;
	int i;
	int fb_id;
	drmModeModeInfo kmode;

	TRACE_ENTER();

	/* remove old fb if it exists */
	drmmode_remove_fb(pScrn);

	if (drmmode->fb_id == 0) {
		unsigned int pitch =
				OMAPCalculateStride(pScrn->virtualX, pScrn->bitsPerPixel);

		DEBUG_MSG("create framebuffer: %dx%d",
				pScrn->virtualX, pScrn->virtualY);

		ret = drmModeAddFB(drmmode->fd,
				pScrn->virtualX, pScrn->virtualY,
				pScrn->depth, pScrn->bitsPerPixel,
				pitch, omap_bo_handle(pOMAP->scanout),
				&drmmode->fb_id);
		if (ret < 0) {
			// Fixme - improve this error message:
			ErrorF("failed to add fb\n");
			return FALSE;
		}
	}

	/* Save the current mode in case there's a problem: */
	saved_mode = crtc->mode;
	saved_x = crtc->x;
	saved_y = crtc->y;
	saved_rotation = crtc->rotation;

	/* Set the new mode: */
	crtc->mode = *mode;
	crtc->x = x;
	crtc->y = y;
	crtc->rotation = rotation;

	output_ids = calloc(sizeof(uint32_t), xf86_config->num_output);
	if (!output_ids) {
		// Fixme - have an error message?
		ret = FALSE;
		goto done;
	}

	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];
		drmmode_output_private_ptr drmmode_output;

		if (output->crtc != crtc)
			continue;

		drmmode_output = output->driver_private;
		output_ids[output_count] =
				drmmode_output->mode_output->connector_id;
		output_count++;
	}

	if (!xf86CrtcRotate(crtc))
		goto done;

	// Fixme - Intel puts this function here, and Nouveau puts it at the end
	// of this function -> determine what's best for TI'S OMAP4:
	crtc->funcs->gamma_set(crtc, crtc->gamma_red, crtc->gamma_green,
			crtc->gamma_blue, crtc->gamma_size);

	drmmode_ConvertToKMode(crtc->scrn, &kmode, mode);

	fb_id = drmmode->fb_id;
	if (drmmode_crtc->rotate_fb_id) {
		fb_id = drmmode_crtc->rotate_fb_id;
		x = 0;
		y = 0;
	}
	ret = drmModeSetCrtc(drmmode->fd, drmmode_crtc->mode_crtc->crtc_id,
			fb_id, x, y, output_ids, output_count, &kmode);
	if (ret) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				"failed to set mode: %s\n", strerror(-ret));
	} else {
		ret = TRUE;
	}

	// FIXME - DO WE NEED TO CALL TO THE PVR EXA/DRI2 CODE TO UPDATE THEM???

	/* Turn on any outputs on this crtc that may have been disabled: */
	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];

		if (output->crtc != crtc)
			continue;

		drmmode_output_dpms(output, DPMSModeOn);
	}

	// Fixme - Intel puts a call to set GEM sizes here; do we need to do
	// something like that?

	if (pScrn->pScreen)
		xf86_reload_cursors(pScrn->pScreen);

done:
	if (output_ids) {
		free(output_ids);
	}
	if (!ret) {
		/* If there was a problem, resture the old mode: */
		crtc->x = saved_x;
		crtc->y = saved_y;
		crtc->rotation = saved_rotation;
		crtc->mode = saved_mode;
	}

	TRACE_EXIT();
	return ret;
} /* drmmode_set_mode_major() */

static void
drmmode_set_cursor_position (xf86CrtcPtr crtc, int x, int y)
{
#if 0 // Fixme - address this function when we address HW cursor functionality
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;

    drmModeMoveCursor(drmmode->fd, drmmode_crtc->mode_crtc->crtc_id, x, y);
#endif
} /* drmmode_set_cursor_position() */

static void
drmmode_load_cursor_argb(xf86CrtcPtr crtc, CARD32 *image)
{
#if 0 // Fixme - address this function when we address HW cursor functionality
#endif
} /* drmmode_load_cursor_argb() */

static void
drmmode_hide_cursor(xf86CrtcPtr crtc)
{
#if 0 // Fixme - address this function when we address HW cursor functionality
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;

    drmModeSetCursor(drmmode->fd, drmmode_crtc->mode_crtc->crtc_id,
                     0, 64, 64);
    drmmode_crtc->cursor_visible = FALSE;
#endif
} /* drmmode_hide_cursor() */

static void
drmmode_show_cursor(xf86CrtcPtr crtc)
{
#if 0 // Fixme - address this function when we address HW cursor functionality
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;

    // Fixme - Do we may need a different data structure for the cursor handle?:
    drmModeSetCursor(drmmode->fd, drmmode_crtc->mode_crtc->crtc_id,
                     drmmode_crtc->cursor->handle, 64, 64);

    drmmode_crtc->cursor_visible = TRUE;
#endif
} /* drmmode_show_cursor() */

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

static const xf86CrtcFuncsRec drmmode_crtc_funcs = {
	.dpms = drmmode_crtc_dpms,
	.set_mode_major = drmmode_set_mode_major,
	.set_cursor_position = drmmode_set_cursor_position,
	.show_cursor = drmmode_show_cursor,
	.hide_cursor = drmmode_hide_cursor,
	.load_cursor_argb = drmmode_load_cursor_argb,
	.gamma_set = drmmode_gamma_set,
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

    drmmode_crtc = xnfcalloc(sizeof(drmmode_crtc_private_rec), 1);
    drmmode_crtc->mode_crtc = drmModeGetCrtc(drmmode->fd,
					     drmmode->mode_res->crtcs[num]);
    drmmode_crtc->drmmode = drmmode;

// FIXME - potentially add code to allocate a HW cursor here.

    crtc->driver_private = drmmode_crtc;

    TRACE_EXIT();
    return;
} /* drmmode_crtc_init() */

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
	drmModePropertyPtr props;
	xf86MonPtr ddc_mon = NULL;
	int i;

	/* look for an EDID property */
	for (i = 0; i < koutput->count_props; i++) {
		props = drmModeGetProperty(drmmode->fd, koutput->props[i]);
		if (!props || !(props->flags & DRM_MODE_PROP_BLOB))
			continue;

		if (!strcmp(props->name, "EDID")) {
			if (drmmode_output->edid_blob)
				drmModeFreePropertyBlob(drmmode_output->edid_blob);
			drmmode_output->edid_blob =
				drmModeGetPropertyBlob(drmmode->fd,
						       koutput->prop_values[i]);
		}
		drmModeFreeProperty(props);
	}

	if (drmmode_output->edid_blob)
		ddc_mon = xf86InterpretEDID(pScrn->scrnIndex,
					    drmmode_output->edid_blob->data);

	if (ddc_mon) {
        XF86_CRTC_CONFIG_PTR(pScrn)->debug_modes = TRUE;
		xf86PrintEDID(ddc_mon);
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
	drmModeFreeConnector(drmmode_output->mode_output);
	free(drmmode_output);
	output->driver_private = NULL;
}

static void
drmmode_output_dpms(xf86OutputPtr output, int mode)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmModeConnectorPtr koutput = drmmode_output->mode_output;
	drmModePropertyPtr props;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	int mode_id = -1, i;

	for (i = 0; i < koutput->count_props; i++) {
		props = drmModeGetProperty(drmmode->fd, koutput->props[i]);
		if (props && (props->flags && DRM_MODE_PROP_ENUM)) {
			if (!strcmp(props->name, "DPMS")) {
				mode_id = koutput->props[i];
				drmModeFreeProperty(props);
				break;
			}
			drmModeFreeProperty(props);
		}
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

	drmmode_output->props = calloc(mode_output->count_props, sizeof(drmmode_prop_rec));
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
			p->atoms = calloc(p->num_atoms, sizeof(Atom));
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
			p->atoms = calloc(p->num_atoms, sizeof(Atom));
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

    drmmode_output = calloc(sizeof(drmmode_output_private_rec), 1);
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
#if 0
// FIXME - check with Rob whether this is supported for OMAP4:
    output->doubleScanAllowed = TRUE;
#endif

    TRACE_EXIT();
    return;
} /* drmmode_output_init() */

static Bool
drmmode_xf86crtc_resize(ScrnInfoPtr pScrn, int width, int height)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	ScreenPtr pScreen = pScrn->pScreen;
	unsigned int pitch;

	TRACE_ENTER();

	/* if fb required size has changed, realloc! */

	DEBUG_MSG("Resize!  %dx%d", width, height);

	pScrn->virtualX = width;
	pScrn->virtualY = height;

	pitch = OMAPCalculateStride(width, pScrn->bitsPerPixel);

	if ((pitch * height) != omap_bo_size(pOMAP->scanout)) {
		/* hmm, should we remove fb here.. we don't want to keep
		 * scanning out a deallocated buffer..
		 */
		drmmode_remove_fb(pScrn);

		/* delete old scanout buffer */
		omap_bo_del(pOMAP->scanout);

		DEBUG_MSG("allocating new scanout buffer: %dx%d (%d)",
				width, height, pitch);

		/* allocate new scanout buffer */
		pOMAP->scanout = omap_bo_new(pOMAP->dev, height * pitch,
				OMAP_BO_SCANOUT | OMAP_BO_WC);
		if (!pOMAP->scanout) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
					"Error reallocating scanout buffer\n");
			return FALSE;
		}
	}

	if (pScreen && pScreen->ModifyPixmapHeader) {
		PixmapPtr rootPixmap = pScreen->GetScreenPixmap(pScreen);
		pScreen->ModifyPixmapHeader(rootPixmap,
				pScrn->virtualX, pScrn->virtualY,
				pScrn->depth, pScrn->bitsPerPixel, pitch,
				omap_bo_map(pOMAP->scanout));
	}

	TRACE_EXIT();
	return TRUE;
} /* drmmode_xf86crtc_resize() */


static const xf86CrtcConfigFuncsRec drmmode_xf86crtc_config_funcs = {
	drmmode_xf86crtc_resize
};


Bool drmmode_pre_init(ScrnInfoPtr pScrn, int fd, int cpp)
{
    xf86CrtcConfigPtr xf86_config;
    drmmode_ptr drmmode;
    int i;

    TRACE_ENTER();

    drmmode = malloc(sizeof *drmmode);
    drmmode->fd = fd;
    drmmode->fb_id = 0;

    xf86CrtcConfigInit(pScrn, &drmmode_xf86crtc_config_funcs);
    xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);


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
} /* drmmode_pre_init() */

void
drmmode_adjust_frame(ScrnInfoPtr pScrn, int x, int y, int flags)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output = config->output[config->compat_output];
	xf86CrtcPtr crtc = output->crtc;

	if (!crtc || !crtc->enabled)
		return;

	drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation, x, y);
}

void
drmmode_remove_fb(ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86CrtcPtr crtc = NULL;
	drmmode_crtc_private_ptr drmmode_crtc;
	drmmode_ptr drmmode;

	if (config)
		crtc = config->crtc[0];
	if (!crtc)
		return;

	drmmode_crtc = crtc->driver_private;
	drmmode = drmmode_crtc->drmmode;

	if (drmmode->fb_id)
		drmModeRmFB(drmmode->fd, drmmode->fb_id);
	drmmode->fb_id = 0;
}


/*
 * Hot Plug Event handling:
 */

static drmmode_ptr
drmmode_from_scrn(ScrnInfoPtr pScrn)
{
    xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
    drmmode_crtc_private_ptr drmmode_crtc;

    drmmode_crtc = xf86_config->crtc[0]->driver_private;
    return drmmode_crtc->drmmode;
} /* drmmode_from_scrn() */

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
    fstat(pOMAP->drmFD, &s);

    hotplug = udev_device_get_property_value(dev, "HOTPLUG");

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "hotplug=%s, match=%d\n", hotplug,
               memcmp(&s.st_rdev, &udev_devnum, sizeof (dev_t)));

    if (memcmp(&s.st_rdev, &udev_devnum, sizeof (dev_t)) == 0 &&
        hotplug && atoi(hotplug) == 1) {
        RRGetInfo(screenInfo.screens[pScrn->scrnIndex], TRUE);
    }
    udev_device_unref(dev);
} /* drmmode_handle_uevents() */

void
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
} /* drmmode_uevent_init() */

void
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
} /* drmmode_uevent_fini() */