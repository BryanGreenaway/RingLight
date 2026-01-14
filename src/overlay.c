/*
 * RingLight Overlay - Pure Wayland layer-shell overlay
 * 
 * Uses wlr-layer-shell protocol directly for proper multi-monitor support.
 * Click anywhere on the overlay to close.
 * License: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pwd.h>
#include <time.h>

#include <wayland-client.h>
#include "xdg-shell-client.h"
#include "wlr-layer-shell-unstable-v1-client.h"

/* ========== Configuration ========== */

static int cfg_border_width = 80;
static int cfg_brightness = 100;
static uint32_t cfg_color = 0xFFFFFF;
static bool cfg_fullscreen = false;
static char cfg_target_name[64] = "";
static bool cfg_list_only = false;
static bool cfg_verbose = false;

/* ========== Wayland State ========== */

static struct wl_display *wl_display;
static struct wl_registry *wl_registry;
static struct wl_compositor *wl_compositor;
static struct wl_shm *wl_shm;
static struct wl_seat *wl_seat;
static struct wl_pointer *wl_pointer;
static struct zwlr_layer_shell_v1 *layer_shell;

static volatile sig_atomic_t running = 1;

/* ========== Output Tracking ========== */

#define MAX_OUTPUTS 8

typedef struct {
    struct wl_output *wl_output;
    char name[64];
    int32_t width, height;
    int32_t x, y;
    int32_t scale;
    bool done;
} output_t;

static output_t outputs[MAX_OUTPUTS];
static int num_outputs = 0;

/* ========== Panel (Layer Surface) ========== */

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

/* ========== Helpers ========== */

#define LOG(...) do { if (cfg_verbose) fprintf(stderr, "[ringlight] " __VA_ARGS__); } while(0)
#define ERR(...) fprintf(stderr, "[ringlight] ERROR: " __VA_ARGS__)

static void sig_handler(int sig) { (void)sig; running = 0; }

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
    
    /* Fill with color (ARGB8888) */
    uint32_t r = ((cfg_color >> 16) & 0xFF) * cfg_brightness / 100;
    uint32_t g = ((cfg_color >> 8) & 0xFF) * cfg_brightness / 100;
    uint32_t b = (cfg_color & 0xFF) * cfg_brightness / 100;
    uint32_t pixel = (0xFFu << 24) | (r << 16) | (g << 8) | b;
    
    uint32_t *pixels = data;
    size_t count = panel->width * panel->height;
    for (size_t i = 0; i < count; i++) pixels[i] = pixel;
    
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, size);
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
    LOG("Created buffer %ux%u\n", panel->width, panel->height);
    return true;
}

static void destroy_panel_buffer(panel_t *panel) {
    if (panel->buffer) { wl_buffer_destroy(panel->buffer); panel->buffer = NULL; }
    if (panel->buffer_data) { munmap(panel->buffer_data, panel->buffer_size); panel->buffer_data = NULL; }
}

/* ========== Pointer Callbacks ========== */

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
            LOG("Click on panel %d - quitting\n", i);
            running = 0;
            return;
        }
    }
}

/* Unused but required by listener */
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

/* ========== Seat Callbacks ========== */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl_pointer) {
        wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl_pointer, &pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl_pointer) {
        wl_pointer_destroy(wl_pointer);
        wl_pointer = NULL;
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ========== Layer Surface Callbacks ========== */

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

/* ========== Output Callbacks ========== */

static output_t *find_output(struct wl_output *wl) {
    for (int i = 0; i < num_outputs; i++)
        if (outputs[i].wl_output == wl) return &outputs[i];
    return NULL;
}

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                           int32_t pw, int32_t ph, int32_t sp, const char *mk,
                           const char *md, int32_t tr) {
    (void)d; (void)pw; (void)ph; (void)sp; (void)mk; (void)md; (void)tr;
    output_t *out = find_output(o);
    if (out) { out->x = x; out->y = y; }
}

static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r) {
    (void)d; (void)r;
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    output_t *out = find_output(o);
    if (out) { out->width = w; out->height = h; }
}

static void output_done(void *d, struct wl_output *o) {
    (void)d;
    output_t *out = find_output(o);
    if (out) out->done = true;
}

static void output_scale(void *d, struct wl_output *o, int32_t f) {
    (void)d;
    output_t *out = find_output(o);
    if (out) out->scale = f;
}

static void output_name(void *d, struct wl_output *o, const char *name) {
    (void)d;
    output_t *out = find_output(o);
    if (out && name) strncpy(out->name, name, sizeof(out->name) - 1);
}

static void output_desc(void *d, struct wl_output *o, const char *desc) { (void)d; (void)o; (void)desc; }

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done,
    .scale = output_scale, .name = output_name, .description = output_desc,
};

/* ========== Registry Callbacks ========== */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                           const char *iface, uint32_t ver) {
    (void)data;
    
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        wl_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        wl_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        wl_seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(wl_seat, &seat_listener, NULL);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (num_outputs < MAX_OUTPUTS) {
            output_t *out = &outputs[num_outputs++];
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

/* ========== Panel Creation ========== */

static panel_t *create_panel(output_t *output, uint32_t anchor, uint32_t w, uint32_t h) {
    panel_t *panel = calloc(1, sizeof(panel_t));
    if (!panel) return NULL;
    
    panel->width = w;
    panel->height = h;
    
    panel->wl_surface = wl_compositor_create_surface(wl_compositor);
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

/* ========== Config Loading ========== */

static char *get_config_value(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    
    static char value[256];
    char line[512];
    size_t keylen = strlen(key);
    
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        /* Trim key */
        char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        char *kend = eq;
        while (kend > k && (*(kend-1) == ' ' || *(kend-1) == '\t')) kend--;
        
        if ((size_t)(kend - k) != keylen || strncmp(k, key, keylen) != 0) continue;
        
        /* Trim value */
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *vend = v + strlen(v);
        while (vend > v && (*(vend-1) == '\n' || *(vend-1) == '\r' || *(vend-1) == ' ')) vend--;
        *vend = '\0';
        
        /* Strip quotes */
        if (vend - v >= 2 && v[0] == '"' && *(vend-1) == '"') {
            v++;
            *(vend-1) = '\0';
        }
        
        strncpy(value, v, sizeof(value) - 1);
        fclose(f);
        return value;
    }
    fclose(f);
    return NULL;
}

static void load_config(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return;
    
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/ringlight/config.ini", home);
    
    char *v;
    if ((v = get_config_value(path, "width"))) {
        cfg_border_width = atoi(v);
        if (cfg_border_width < 1) cfg_border_width = 1;
        if (cfg_border_width > 500) cfg_border_width = 500;
    }
    if ((v = get_config_value(path, "brightness"))) {
        cfg_brightness = atoi(v);
        if (cfg_brightness < 1) cfg_brightness = 1;
        if (cfg_brightness > 100) cfg_brightness = 100;
    }
    if ((v = get_config_value(path, "color"))) {
        if (v[0] == '#') v++;
        cfg_color = strtoul(v, NULL, 16);
    }
    if ((v = get_config_value(path, "fullscreen"))) {
        cfg_fullscreen = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
}

/* ========== Main ========== */

static void print_usage(const char *prog) {
    printf("ringlight-overlay - Screen ring light for Wayland\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("  -s, --screen N|NAME  Screen index or name\n");
    printf("  -w, --width N        Border width in pixels (default: 80)\n");
    printf("  -c, --color RRGGBB   Color in hex (default: FFFFFF)\n");
    printf("  -b, --brightness N   Brightness 1-100 (default: 100)\n");
    printf("  -f, --fullscreen     Full screen mode\n");
    printf("  -l, --list           List screens and exit\n");
    printf("  -v, --verbose        Verbose output\n");
    printf("  -h, --help           Show this help\n");
    printf("\nClick on the overlay to close.\n");
}

int main(int argc, char *argv[]) {
    load_config();
    
    static struct option long_opts[] = {
        {"screen", required_argument, 0, 's'},
        {"width", required_argument, 0, 'w'},
        {"color", required_argument, 0, 'c'},
        {"brightness", required_argument, 0, 'b'},
        {"fullscreen", no_argument, 0, 'f'},
        {"list", no_argument, 0, 'l'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "s:w:c:b:flvh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': strncpy(cfg_target_name, optarg, sizeof(cfg_target_name) - 1); break;
        case 'w': cfg_border_width = atoi(optarg); if (cfg_border_width < 1) cfg_border_width = 1; if (cfg_border_width > 500) cfg_border_width = 500; break;
        case 'c': if (optarg[0] == '#') optarg++; cfg_color = strtoul(optarg, NULL, 16); break;
        case 'b': cfg_brightness = atoi(optarg); if (cfg_brightness < 1) cfg_brightness = 1; if (cfg_brightness > 100) cfg_brightness = 100; break;
        case 'f': cfg_fullscreen = true; break;
        case 'l': cfg_list_only = true; break;
        case 'v': cfg_verbose = true; break;
        case 'h': print_usage(argv[0]); return 0;
        default: print_usage(argv[0]); return 1;
        }
    }
    
    wl_display = wl_display_connect(NULL);
    if (!wl_display) { ERR("Failed to connect to Wayland\n"); return 1; }
    
    wl_registry = wl_display_get_registry(wl_display);
    wl_registry_add_listener(wl_registry, &registry_listener, NULL);
    wl_display_roundtrip(wl_display);
    wl_display_roundtrip(wl_display);
    wl_display_roundtrip(wl_display);
    
    if (!wl_compositor || !wl_shm || !layer_shell) {
        ERR("Missing required Wayland interfaces\n");
        return 1;
    }
    
    if (cfg_list_only) {
        printf("Available screens:\n");
        for (int i = 0; i < num_outputs; i++)
            printf("  %d: %s (%dx%d @ %d,%d)\n", i, outputs[i].name,
                   outputs[i].width, outputs[i].height, outputs[i].x, outputs[i].y);
        wl_display_disconnect(wl_display);
        return 0;
    }
    
    if (num_outputs == 0) { ERR("No outputs found\n"); return 1; }
    
    /* Find target output */
    output_t *target = NULL;
    if (cfg_target_name[0]) {
        for (int i = 0; i < num_outputs; i++) {
            if (strcmp(outputs[i].name, cfg_target_name) == 0) {
                target = &outputs[i];
                break;
            }
        }
        if (!target) {
            char *end;
            long idx = strtol(cfg_target_name, &end, 10);
            if (*end == '\0' && idx >= 0 && idx < num_outputs)
                target = &outputs[idx];
        }
        if (!target) {
            ERR("Screen '%s' not found\n", cfg_target_name);
            return 1;
        }
    } else {
        target = &outputs[0];
    }
    
    printf("%s on %s (%dx%d)\n", cfg_fullscreen ? "Fullscreen" : "Ring",
           target->name, target->width, target->height);
    
    /* Create panels */
    if (cfg_fullscreen) {
        uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        panels[num_panels++] = create_panel(target, anchor, 0, 0);
    } else {
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            0, cfg_border_width);
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            0, cfg_border_width);
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            cfg_border_width, 0);
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            cfg_border_width, 0);
    }
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    /* Wait for configure */
    while (running) {
        if (wl_display_dispatch(wl_display) < 0) break;
        bool all_done = true;
        for (int i = 0; i < num_panels; i++)
            if (panels[i] && !panels[i]->configured) { all_done = false; break; }
        if (all_done) break;
    }
    
    /* Main loop */
    while (running) {
        if (wl_display_dispatch(wl_display) < 0) break;
    }
    
    /* Cleanup */
    for (int i = 0; i < num_panels; i++) destroy_panel(panels[i]);
    if (wl_pointer) wl_pointer_destroy(wl_pointer);
    if (wl_seat) wl_seat_destroy(wl_seat);
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (wl_shm) wl_shm_destroy(wl_shm);
    if (wl_compositor) wl_compositor_destroy(wl_compositor);
    for (int i = 0; i < num_outputs; i++)
        if (outputs[i].wl_output) wl_output_destroy(outputs[i].wl_output);
    wl_registry_destroy(wl_registry);
    wl_display_disconnect(wl_display);
    
    return 0;
}
