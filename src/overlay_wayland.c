/*
 * RingLight Wayland Backend - wlr-layer-shell overlay
 *
 * Copyright (C) 2024-2025 Bryan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

#include <wayland-client.h>
#include "xdg-shell-client.h"
#include "wlr-layer-shell-unstable-v1-client.h"

#include "overlay_common.h"

/* Wayland globals */
static struct wl_display *wl_dpy;
static struct wl_registry *wl_reg;
static struct wl_compositor *wl_comp;
static struct wl_shm *wl_shm_global;
static struct wl_seat *wl_seat_global;
static struct wl_pointer *wl_ptr;
static struct zwlr_layer_shell_v1 *layer_shell;

static config_t *g_cfg;

/* Output tracking */
#define MAX_OUTPUTS 8

typedef struct {
    struct wl_output *wl_output;
    char name[64];
    int32_t width, height;
    int32_t x, y;
    int32_t scale;
    bool done;
} wl_output_t;

static wl_output_t outputs[MAX_OUTPUTS];
static int num_outputs = 0;

/* Panel (layer surface) */
typedef struct {
    struct wl_surface *wl_surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;
    void *buffer_data;
    size_t buffer_size;
    uint32_t width, height;
    bool configured;
} panel_t;

#define MAX_PANELS 5
static panel_t *panels[MAX_PANELS];
static int num_panels = 0;
static struct wl_surface *pointer_surface;

/* SHM helpers */
static int create_shm_file(size_t size) {
    int fd = -1;

#ifdef __NR_memfd_create
    fd = syscall(__NR_memfd_create, "ringlight", 0);
#endif

    if (fd < 0) {
        char name[64];
        snprintf(name, sizeof(name), "/ringlight-%d-%ld", getpid(), (long)time(NULL));
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) shm_unlink(name);
    }

    if (fd < 0) {
        ERR("Failed to create shm file: %s\n", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, size) < 0) {
        ERR("Failed to resize shm file: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static bool create_panel_buffer(panel_t *panel) {
    if (panel->width == 0 || panel->height == 0) {
        ERR("Invalid panel dimensions: %ux%u\n", panel->width, panel->height);
        return false;
    }

    int stride = panel->width * 4;
    size_t size = stride * panel->height;

    int fd = create_shm_file(size);
    if (fd < 0) return false;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        ERR("mmap failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    uint32_t pixel = calculate_pixel(g_cfg->color, g_cfg->brightness);
    uint32_t *pixels = data;
    size_t count = panel->width * panel->height;
    for (size_t i = 0; i < count; i++) pixels[i] = pixel;

    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm_global, fd, size);
    close(fd);

    if (!pool) {
        ERR("Failed to create shm pool\n");
        munmap(data, size);
        return false;
    }

    panel->buffer = wl_shm_pool_create_buffer(pool, 0, panel->width, panel->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    if (!panel->buffer) {
        ERR("Failed to create wl_buffer\n");
        munmap(data, size);
        return false;
    }

    panel->buffer_data = data;
    panel->buffer_size = size;
    LOG(g_cfg, "Created buffer %ux%u\n", panel->width, panel->height);
    return true;
}

static void destroy_panel_buffer(panel_t *panel) {
    if (panel->buffer) { wl_buffer_destroy(panel->buffer); panel->buffer = NULL; }
    if (panel->buffer_data) { munmap(panel->buffer_data, panel->buffer_size); panel->buffer_data = NULL; }
}

/* Pointer callbacks */
static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                         struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)data; (void)p; (void)serial; (void)sx; (void)sy;
    pointer_surface = surface;
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface) {
    (void)data; (void)p; (void)serial; (void)surface;
    pointer_surface = NULL;
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
                          uint32_t time, uint32_t button, uint32_t state) {
    (void)data; (void)p; (void)serial; (void)time; (void)button;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;

    for (int i = 0; i < num_panels; i++) {
        if (panels[i] && panels[i]->wl_surface == pointer_surface) {
            LOG(g_cfg, "Click detected - exiting\n");
            running = 0;
            return;
        }
    }
}

static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; }
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static void pointer_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
    .axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* Seat callbacks */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl_ptr) {
        wl_ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl_ptr, &pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl_ptr) {
        wl_pointer_destroy(wl_ptr);
        wl_ptr = NULL;
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* Layer surface callbacks */
static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                           uint32_t serial, uint32_t width, uint32_t height) {
    panel_t *panel = data;

    zwlr_layer_surface_v1_ack_configure(surface, serial);

    if (width > 0) panel->width = width;
    if (height > 0) panel->height = height;

    destroy_panel_buffer(panel);
    if (!create_panel_buffer(panel)) {
        running = 0;
        return;
    }

    wl_surface_attach(panel->wl_surface, panel->buffer, 0, 0);
    wl_surface_damage_buffer(panel->wl_surface, 0, 0, panel->width, panel->height);
    wl_surface_commit(panel->wl_surface);
    panel->configured = true;
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)surface;
    ((panel_t *)data)->configured = false;
    running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

/* Output callbacks */
static wl_output_t *find_output(struct wl_output *wl) {
    for (int i = 0; i < num_outputs; i++)
        if (outputs[i].wl_output == wl) return &outputs[i];
    return NULL;
}

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                           int32_t pw, int32_t ph, int32_t sp, const char *mk,
                           const char *md, int32_t tr) {
    (void)d; (void)pw; (void)ph; (void)sp; (void)mk; (void)md; (void)tr;
    wl_output_t *out = find_output(o);
    if (out) { out->x = x; out->y = y; }
}

static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r) {
    (void)d; (void)r;
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    wl_output_t *out = find_output(o);
    if (out) { out->width = w; out->height = h; }
}

static void output_done(void *d, struct wl_output *o) {
    (void)d;
    wl_output_t *out = find_output(o);
    if (out) out->done = true;
}

static void output_scale(void *d, struct wl_output *o, int32_t f) {
    (void)d;
    wl_output_t *out = find_output(o);
    if (out) out->scale = f;
}

static void output_name(void *d, struct wl_output *o, const char *name) {
    (void)d;
    wl_output_t *out = find_output(o);
    if (out && name) strncpy(out->name, name, sizeof(out->name) - 1);
}

static void output_desc(void *d, struct wl_output *o, const char *desc) { (void)d; (void)o; (void)desc; }

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done,
    .scale = output_scale, .name = output_name, .description = output_desc,
};

/* Registry callbacks */
static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                           const char *iface, uint32_t ver) {
    (void)data;

    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        wl_comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        wl_shm_global = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        wl_seat_global = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(wl_seat_global, &seat_listener, NULL);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (num_outputs < MAX_OUTPUTS) {
            wl_output_t *out = &outputs[num_outputs++];
            memset(out, 0, sizeof(*out));
            out->scale = 1;
            snprintf(out->name, sizeof(out->name), "output-%d", num_outputs - 1);
            out->wl_output = wl_registry_bind(reg, name, &wl_output_interface, ver < 4 ? ver : 4);
            wl_output_add_listener(out->wl_output, &output_listener, NULL);
        }
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
    }
}

static void registry_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

/* Panel creation */
static panel_t *create_panel(wl_output_t *output, uint32_t anchor, uint32_t w, uint32_t h) {
    panel_t *panel = calloc(1, sizeof(panel_t));
    if (!panel) return NULL;

    panel->width = w;
    panel->height = h;

    panel->wl_surface = wl_compositor_create_surface(wl_comp);
    if (!panel->wl_surface) { free(panel); return NULL; }

    panel->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, panel->wl_surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "ringlight");

    if (!panel->layer_surface) {
        wl_surface_destroy(panel->wl_surface);
        free(panel);
        return NULL;
    }

    zwlr_layer_surface_v1_add_listener(panel->layer_surface, &layer_listener, panel);
    zwlr_layer_surface_v1_set_size(panel->layer_surface, w, h);
    zwlr_layer_surface_v1_set_anchor(panel->layer_surface, anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(panel->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(panel->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    wl_surface_commit(panel->wl_surface);
    return panel;
}

static void destroy_panel(panel_t *panel) {
    if (!panel) return;
    destroy_panel_buffer(panel);
    if (panel->layer_surface) zwlr_layer_surface_v1_destroy(panel->layer_surface);
    if (panel->wl_surface) wl_surface_destroy(panel->wl_surface);
    free(panel);
}

/* Connect to Wayland and enumerate outputs */
static int wl_connect(void) {
    wl_dpy = wl_display_connect(NULL);
    if (!wl_dpy) { ERR("Failed to connect to Wayland\n"); return -1; }

    wl_reg = wl_display_get_registry(wl_dpy);
    wl_registry_add_listener(wl_reg, &registry_listener, NULL);
    wl_display_roundtrip(wl_dpy);
    wl_display_roundtrip(wl_dpy);
    wl_display_roundtrip(wl_dpy);

    if (!wl_comp || !wl_shm_global || !layer_shell) {
        ERR("Missing required Wayland interfaces\n");
        return -1;
    }

    return 0;
}

static void wl_cleanup(void) {
    for (int i = 0; i < num_panels; i++) destroy_panel(panels[i]);
    if (wl_ptr) wl_pointer_destroy(wl_ptr);
    if (wl_seat_global) wl_seat_destroy(wl_seat_global);
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (wl_shm_global) wl_shm_destroy(wl_shm_global);
    if (wl_comp) wl_compositor_destroy(wl_comp);
    for (int i = 0; i < num_outputs; i++)
        if (outputs[i].wl_output) wl_output_destroy(outputs[i].wl_output);
    wl_registry_destroy(wl_reg);
    wl_display_disconnect(wl_dpy);
}

/* Public API */
int wayland_list_screens(config_t *cfg) {
    g_cfg = cfg;

    if (wl_connect() < 0) return 1;

    printf("Available screens:\n");
    for (int i = 0; i < num_outputs; i++)
        printf("  %d: %s (%dx%d @ %d,%d)\n", i, outputs[i].name,
               outputs[i].width, outputs[i].height, outputs[i].x, outputs[i].y);

    wl_cleanup();
    return 0;
}

int wayland_run(config_t *cfg) {
    g_cfg = cfg;

    if (wl_connect() < 0) return 1;

    if (num_outputs == 0) { ERR("No outputs found\n"); wl_cleanup(); return 1; }

    /* Find target output */
    wl_output_t *target = NULL;
    if (cfg->target_name[0]) {
        for (int i = 0; i < num_outputs; i++) {
            if (strcmp(outputs[i].name, cfg->target_name) == 0) {
                target = &outputs[i];
                break;
            }
        }
        if (!target) {
            char *end;
            long idx = strtol(cfg->target_name, &end, 10);
            if (*end == '\0' && idx >= 0 && idx < num_outputs)
                target = &outputs[idx];
        }
        if (!target) {
            ERR("Screen '%s' not found\n", cfg->target_name);
            wl_cleanup();
            return 1;
        }
    } else {
        target = &outputs[0];
    }

    LOG(cfg, "Overlay on %s (%dx%d), %s mode\n", target->name, target->width, target->height,
        cfg->fullscreen ? "fullscreen" : "ring");

    /* Create panels */
    if (cfg->fullscreen) {
        uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        panels[num_panels++] = create_panel(target, anchor, 0, 0);
    } else {
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            0, cfg->border_width);
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            0, cfg->border_width);
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            cfg->border_width, 0);
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            cfg->border_width, 0);
    }

    /* Wait for configure */
    while (running) {
        if (wl_display_dispatch(wl_dpy) < 0) break;
        bool all_done = true;
        for (int i = 0; i < num_panels; i++)
            if (panels[i] && !panels[i]->configured) { all_done = false; break; }
        if (all_done) break;
    }

    /* Main loop */
    int wl_fd = wl_display_get_fd(wl_dpy);
    while (running) {
        while (wl_display_prepare_read(wl_dpy) != 0)
            wl_display_dispatch_pending(wl_dpy);
        wl_display_flush(wl_dpy);

        struct pollfd pfd = { .fd = wl_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 100);

        if (ret < 0) {
            wl_display_cancel_read(wl_dpy);
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            wl_display_read_events(wl_dpy);
            wl_display_dispatch_pending(wl_dpy);
        } else {
            wl_display_cancel_read(wl_dpy);
        }
    }

    wl_cleanup();
    return 0;
}
