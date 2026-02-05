/*
 * RingLight X11 Backend - Xlib with XRandR
 *
 * Copyright (C) 2024-2025 Bryan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <poll.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include "overlay_common.h"

#define MAX_OUTPUTS 8
#define MAX_PANELS 5

typedef struct {
    char name[64];
    int32_t x, y;
    int32_t width, height;
} x11_output_t;

static Display *dpy;
static Window panels[MAX_PANELS];
static int num_panels = 0;
static x11_output_t outputs[MAX_OUTPUTS];
static int num_outputs = 0;

/* Enumerate connected monitors via XRandR */
static int enumerate_screens(void) {
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    XRRScreenResources *res = XRRGetScreenResources(dpy, root);
    if (!res) {
        ERR("Failed to get XRandR screen resources\n");
        return -1;
    }

    num_outputs = 0;
    for (int i = 0; i < res->noutput && num_outputs < MAX_OUTPUTS; i++) {
        XRROutputInfo *info = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!info) continue;

        if (info->connection == RR_Connected && info->crtc) {
            XRRCrtcInfo *crtc = XRRGetCrtcInfo(dpy, res, info->crtc);
            if (crtc) {
                x11_output_t *out = &outputs[num_outputs++];
                strncpy(out->name, info->name, sizeof(out->name) - 1);
                out->name[sizeof(out->name) - 1] = '\0';
                out->x = crtc->x;
                out->y = crtc->y;
                out->width = crtc->width;
                out->height = crtc->height;
                XRRFreeCrtcInfo(crtc);
            }
        }
        XRRFreeOutputInfo(info);
    }

    XRRFreeScreenResources(res);
    return 0;
}

/* Create an override-redirect window at the given position */
static Window create_panel_window(int x, int y, int width, int height, unsigned long pixel) {
    int screen = DefaultScreen(dpy);

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = pixel;
    attrs.event_mask = ButtonPressMask;

    Window win = XCreateWindow(dpy, RootWindow(dpy, screen),
        x, y, width, height, 0,
        DefaultDepth(dpy, screen), InputOutput, DefaultVisual(dpy, screen),
        CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);

    return win;
}

/* Allocate the overlay color */
static unsigned long alloc_color(config_t *cfg) {
    int screen = DefaultScreen(dpy);
    Colormap cmap = DefaultColormap(dpy, screen);

    XColor xc;
    xc.red   = (((cfg->color >> 16) & 0xFF) * cfg->brightness / 100) * 257;
    xc.green = (((cfg->color >> 8)  & 0xFF) * cfg->brightness / 100) * 257;
    xc.blue  = ((cfg->color         & 0xFF) * cfg->brightness / 100) * 257;
    xc.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, cmap, &xc);

    return xc.pixel;
}

/* Find target output by name or index */
static x11_output_t *find_target(config_t *cfg) {
    if (!cfg->target_name[0])
        return &outputs[0];

    for (int i = 0; i < num_outputs; i++) {
        if (strcmp(outputs[i].name, cfg->target_name) == 0)
            return &outputs[i];
    }

    char *end;
    long idx = strtol(cfg->target_name, &end, 10);
    if (*end == '\0' && idx >= 0 && idx < num_outputs)
        return &outputs[idx];

    return NULL;
}

/* Public API */
int x11_list_screens(config_t *cfg) {
    (void)cfg;

    dpy = XOpenDisplay(NULL);
    if (!dpy) { ERR("Failed to open X display\n"); return 1; }

    if (enumerate_screens() < 0) {
        XCloseDisplay(dpy);
        return 1;
    }

    printf("Available screens:\n");
    for (int i = 0; i < num_outputs; i++)
        printf("  %d: %s (%dx%d @ %d,%d)\n", i, outputs[i].name,
               outputs[i].width, outputs[i].height, outputs[i].x, outputs[i].y);

    XCloseDisplay(dpy);
    return 0;
}

int x11_run(config_t *cfg) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) { ERR("Failed to open X display\n"); return 1; }

    if (enumerate_screens() < 0 || num_outputs == 0) {
        ERR("No screens found\n");
        XCloseDisplay(dpy);
        return 1;
    }

    x11_output_t *target = find_target(cfg);
    if (!target) {
        ERR("Screen '%s' not found\n", cfg->target_name);
        XCloseDisplay(dpy);
        return 1;
    }

    LOG(cfg, "Overlay on %s (%dx%d), %s mode\n", target->name,
        target->width, target->height, cfg->fullscreen ? "fullscreen" : "ring");

    unsigned long pixel = alloc_color(cfg);
    int bw = cfg->border_width;

    if (cfg->fullscreen) {
        panels[num_panels++] = create_panel_window(
            target->x, target->y, target->width, target->height, pixel);
    } else {
        /* Top */
        panels[num_panels++] = create_panel_window(
            target->x, target->y, target->width, bw, pixel);
        /* Bottom */
        panels[num_panels++] = create_panel_window(
            target->x, target->y + target->height - bw, target->width, bw, pixel);
        /* Left */
        panels[num_panels++] = create_panel_window(
            target->x, target->y, bw, target->height, pixel);
        /* Right */
        panels[num_panels++] = create_panel_window(
            target->x + target->width - bw, target->y, bw, target->height, pixel);
    }

    for (int i = 0; i < num_panels; i++)
        XMapRaised(dpy, panels[i]);
    XFlush(dpy);

    /* Event loop */
    int x_fd = ConnectionNumber(dpy);
    while (running) {
        struct pollfd pfd = { .fd = x_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 100);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == ButtonPress) {
                for (int i = 0; i < num_panels; i++) {
                    if (ev.xbutton.window == panels[i]) {
                        LOG(cfg, "Click detected - exiting\n");
                        running = 0;
                        break;
                    }
                }
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < num_panels; i++)
        XDestroyWindow(dpy, panels[i]);
    XCloseDisplay(dpy);

    return 0;
}
