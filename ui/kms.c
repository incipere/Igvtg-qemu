/*
 * QEMU Intel GVT-g indirect display support
 *
 * Copyright (c) Intel
 *
 * Authors:
 *  Tina Zhang   <tina.zhang@intel.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "sysemu/sysemu.h"
#include "ui/console.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <sys/ioctl.h>

#include "drm_fourcc.h"

struct intel_vgpu_display_pipe {
    /* userspace drm_framebuffer id */
    uint32_t primary_fb_id; //standing for a vGPU's primary plane
    uint32_t cursor_fb_id; //standing for a vGPU's cursor plane

    /* info of the assigned HW display engine resources */
    uint32_t primary_plane_id;
    uint32_t cursor_plane_id;
    int crtc_id;
};

struct intel_vgpu_display {
    struct intel_vgpu_display_pipe pipe;
} intel_vgpu_display;

/* Each modeset_dev stands for a connected display monitor */
struct modeset_monitor {
    uint32_t width;
    uint32_t height;
    drmModeModeInfo mode;
    uint32_t conn; //connection id
    uint32_t crtc; //crtc id

    int fd;
} modeset_dev;

static DisplayChangeListener *dcl;

static bool kmstest_get_property(int drm_fd, uint32_t object_id, uint32_t object_type,
		     const char *name, uint32_t *prop_id /* out */,
		     uint64_t *value /* out */,
		     drmModePropertyPtr *prop /* out */)
{
	drmModeObjectPropertiesPtr proplist;
	drmModePropertyPtr _prop;
	bool found = false;
	int i;

	proplist = drmModeObjectGetProperties(drm_fd, object_id, object_type);
	for (i = 0; i < proplist->count_props; i++) {
		_prop = drmModeGetProperty(drm_fd, proplist->props[i]);
		if (!_prop)
			continue;

		if (strcmp(_prop->name, name) == 0) {
			found = true;
			if (prop_id)
				*prop_id = proplist->props[i];
			if (value)
				*value = proplist->prop_values[i];
			if (prop)
				*prop = _prop;
			else
				drmModeFreeProperty(_prop);

			break;
		}
		drmModeFreeProperty(_prop);
	}

	drmModeFreeObjectProperties(proplist);
	return found;
}

static int get_drm_plane_type(int drm_fd, uint32_t plane_id)
{
	uint64_t value;
	bool has_prop;

	has_prop = kmstest_get_property(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE,
					"type", NULL, &value, NULL);
	if (has_prop)
		return (int)value;

	return DRM_PLANE_TYPE_OVERLAY;
}

static void dump_drmModePlaneRes_info(drmModePlaneRes *res)
{
	int i;

	for (i = 0; i < res->count_planes; i++)
		fprintf(stderr, "plane_id is %d\n", res->planes[i]);
}

static void kms_assign_planes(void)
{
    drmModePlaneRes *plane_resources;
    int i, type;

    plane_resources = drmModeGetPlaneResources(modeset_dev.fd);

    for (i = 0; i < plane_resources->count_planes; i++) {
        drmModePlane *drm_plane;

        drm_plane = drmModeGetPlane(modeset_dev.fd, plane_resources->planes[i]);

        if (!drm_plane || !(drm_plane->possible_crtcs & (1 << i))) {
            drmModeFreePlane(drm_plane);
            continue;
        }

        type = get_drm_plane_type(modeset_dev.fd,
                                  plane_resources->planes[i]);

        if (type == DRM_PLANE_TYPE_PRIMARY) {
            intel_vgpu_display.pipe.primary_plane_id = plane_resources->planes[i];
            drmModeFreePlane(drm_plane);
            break;
        }
        drmModeFreePlane(drm_plane);
    }

    dump_drmModePlaneRes_info(plane_resources);
    drmModeFreePlaneResources(plane_resources);

    if (!intel_vgpu_display.pipe.primary_plane_id)
        fprintf(stderr, "cannot set plane for drm_framebuffer %u (%d): %m\n",
                intel_vgpu_display.pipe.primary_fb_id, errno);

    intel_vgpu_display.pipe.crtc_id = modeset_dev.crtc;

    /* Set planes */
    drmModeSetPlane(modeset_dev.fd, intel_vgpu_display.pipe.primary_plane_id,
                          intel_vgpu_display.pipe.crtc_id, intel_vgpu_display.pipe.primary_fb_id, 0, 0, 0, 1920, 1200,
                          0, 0, 1920 << 16, 1200 << 16);

}

static void kms_refresh(DisplayChangeListener *dcl)
{
    ;
}

static void kms_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
    ;
}

static void kms_gfx_switch(DisplayChangeListener *dcl,
                           struct DisplaySurface *new_surface)
{
    ;
}

static const DisplayChangeListenerOps kms_ops = {
    .dpy_name                = "kms",
    .dpy_refresh             = kms_refresh,
    .dpy_gfx_update          = kms_gfx_update,
    .dpy_gfx_switch          = kms_gfx_switch,
};

static int kms_rendernode_init(const char *rendernode)
{
    const char *card = "/dev/dri/card0";
    drmModeRes *res;
    drmModeConnector *connector;
    unsigned int i;
    drmModeEncoder *enc;
    int ret, fd;

    /* Open card */
    fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ret = -errno;
        printf("KMS: cannot open card0 \n");
        return ret;
    }

    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    res = drmModeGetResources(fd);
    if (!res) {
	fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
		errno);
	return -errno;
    }

    /* Iterate all connectors and record the connected ones */
    for (i = 0; i < res->count_connectors; ++i) {
	connector = drmModeGetConnector(fd, res->connectors[i]);
	if (!connector) {
            fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
                    i, res->connectors[i], errno);
            drmModeFreeConnector(connector);
            continue;
	}

	if (connector->connection != DRM_MODE_CONNECTED) {
            fprintf(stderr, "ignoring unused connector %u\n",
                    connector->connector_id);
            drmModeFreeConnector(connector);
            continue;
	}

	if (connector->count_modes == 0) {
            fprintf(stderr, "no valid mode for connector %u\n",
                    connector->connector_id);
            drmModeFreeConnector(connector);
            continue;
	}

        if (connector->encoder_id) {
            enc = drmModeGetEncoder(fd, connector->encoder_id);
        } else {
            drmModeFreeConnector(connector);
            continue;
        }

        /* find the crtc id */
        if (!enc->crtc_id) {
            drmModeFreeConnector(connector);
            continue;
        }

        modeset_dev.crtc = enc->crtc_id;
        modeset_dev.conn = connector->connector_id;

        drmModeFreeEncoder(enc);
	drmModeFreeConnector(connector);

        modeset_dev.fd = fd;

        break;
    }
    drmModeFreeResources(res);
    return 0;
}

static void kms_init(DisplayState *ds, DisplayOptions *opts)
{
    uint32_t vgpu_primary_plane_drm_id;

    if (kms_rendernode_init(NULL) < 0) {
        error_report("kms: render node init failed");
        exit(1);
    }

    dcl = g_new0(DisplayChangeListener, 1);
    dcl->ops = &kms_ops;
    register_displaychangelistener(dcl);

    vgpu_primary_plane_drm_id = graphic_get_plane_id(dcl->con, DRM_PLANE_TYPE_PRIMARY);
    if (!vgpu_primary_plane_drm_id) {
        error_report("kms: get vgpu's primary failed\n");
        unregister_displaychangelistener(dcl);
        g_free(dcl);
        exit(1);
    }

    intel_vgpu_display.pipe.primary_fb_id = vgpu_primary_plane_drm_id;

    kms_assign_planes();
}

static QemuDisplay qemu_display_kms = {
    .type       = DISPLAY_TYPE_KMS,
    .init       = kms_init,
};

static void register_kms(void)
{
    qemu_display_register(&qemu_display_kms);
}

type_init(register_kms);
