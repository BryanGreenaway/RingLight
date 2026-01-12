/*
 * RingLight Overlay - Pure Wayland layer-shell overlay
 * 
 * Uses wlr-layer-shell protocol directly for proper multi-monitor support.
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
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pwd.h>
#include <time.h>

#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client.h"

/* ========== Configuration ========== */

static int cfg_border_width = 80;
static int cfg_brightness = 100;
static uint32_t cfg_color = 0xFFFFFF;  // RGB
static bool cfg_fullscreen = false;
static int cfg_target_output = -1;  // -1 = use first/default
static char cfg_target_name[64] = "";
static bool cfg_list_only = false;
static bool cfg_verbose = false;

/* ========== Wayland State ========== */

static struct wl_display *wl_display = NULL;
static struct wl_registry *wl_registry = NULL;
static struct wl_compositor *wl_compositor = NULL;
static struct wl_shm *wl_shm = NULL;
static struct zwlr_layer_shell_v1 *layer_shell = NULL;

static volatile sig_atomic_t running = 1;

/* ========== Output Tracking ========== */

#define MAX_OUTPUTS 8

typedef struct {
    struct wl_output *wl_output;
    uint32_t global_name;
    char name[64];
    int32_t width;
    int32_t height;
    int32_t x;
    int32_t y;
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
    uint32_t width;
    uint32_t height;
    uint32_t anchor;
    bool configured;
    bool closed;
} panel_t;

#define MAX_PANELS 5
static panel_t *panels[MAX_PANELS];
static int num_panels = 0;

/* ========== Helpers ========== */

#define LOG(...) do { if (cfg_verbose) fprintf(stderr, "[ringlight] " __VA_ARGS__); } while(0)
#define ERR(...) fprintf(stderr, "[ringlight] ERROR: " __VA_ARGS__)

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

/* Create anonymous shared memory file */
static int create_shm_file(size_t size) {
    int fd = -1;
    
    /* Try memfd_create first (Linux 3.17+) */
#ifdef __NR_memfd_create
    fd = syscall(__NR_memfd_create, "ringlight-buffer", 0);
#endif
    
    /* Fallback to shm_open */
    if (fd < 0) {
        char name[64];
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        snprintf(name, sizeof(name), "/ringlight-%d-%ld", getpid(), ts.tv_nsec);
        
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
        }
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

/* Create a buffer filled with solid color */
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
    
    /* Fill with color (ARGB8888 format) */
    uint32_t r = ((cfg_color >> 16) & 0xFF) * cfg_brightness / 100;
    uint32_t g = ((cfg_color >> 8) & 0xFF) * cfg_brightness / 100;
    uint32_t b = (cfg_color & 0xFF) * cfg_brightness / 100;
    uint32_t pixel = (0xFF << 24) | (r << 16) | (g << 8) | b;
    
    uint32_t *pixels = data;
    for (size_t i = 0; i < panel->width * panel->height; i++) {
        pixels[i] = pixel;
    }
    
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, size);
    if (!pool) {
        ERR("Failed to create shm pool\n");
        munmap(data, size);
        close(fd);
        return false;
    }
    
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, panel->width, panel->height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    
    if (!buffer) {
        ERR("Failed to create wl_buffer\n");
        munmap(data, size);
        return false;
    }
    
    panel->buffer = buffer;
    panel->buffer_data = data;
    panel->buffer_size = size;
    
    LOG("Created buffer %ux%u (%zu bytes)\n", panel->width, panel->height, size);
    return true;
}

static void destroy_panel_buffer(panel_t *panel) {
    if (panel->buffer) {
        wl_buffer_destroy(panel->buffer);
        panel->buffer = NULL;
    }
    if (panel->buffer_data) {
        munmap(panel->buffer_data, panel->buffer_size);
        panel->buffer_data = NULL;
        panel->buffer_size = 0;
    }
}

/* ========== Layer Surface Callbacks ========== */

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    panel_t *panel = data;
    
    LOG("Configure: serial=%u size=%ux%u\n", serial, width, height);
    
    /* Acknowledge the configure */
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    
    /* Update size if compositor provided one */
    if (width > 0) panel->width = width;
    if (height > 0) panel->height = height;
    
    /* Recreate buffer if needed */
    if (!panel->buffer || 
        (width > 0 && height > 0 && 
         (panel->width != width || panel->height != height))) {
        destroy_panel_buffer(panel);
        if (!create_panel_buffer(panel)) {
            ERR("Failed to create buffer in configure\n");
            running = 0;
            return;
        }
    }
    
    /* Attach and commit */
    wl_surface_attach(panel->wl_surface, panel->buffer, 0, 0);
    wl_surface_damage_buffer(panel->wl_surface, 0, 0, panel->width, panel->height);
    wl_surface_commit(panel->wl_surface);
    
    panel->configured = true;
    LOG("Panel configured and committed\n");
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    panel_t *panel = data;
    (void)surface;
    LOG("Layer surface closed\n");
    panel->closed = true;
    running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* ========== Output Callbacks ========== */

static output_t *find_output_by_wl(struct wl_output *wl_output) {
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].wl_output == wl_output) return &outputs[i];
    }
    return NULL;
}

static void output_geometry(void *data, struct wl_output *wl_output,
                           int32_t x, int32_t y, int32_t phys_w, int32_t phys_h,
                           int32_t subpixel, const char *make, const char *model,
                           int32_t transform) {
    (void)data; (void)phys_w; (void)phys_h; (void)subpixel; 
    (void)make; (void)model; (void)transform;
    
    output_t *output = find_output_by_wl(wl_output);
    if (output) {
        output->x = x;
        output->y = y;
    }
}

static void output_mode(void *data, struct wl_output *wl_output,
                        uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)data; (void)refresh;
    
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    
    output_t *output = find_output_by_wl(wl_output);
    if (output) {
        output->width = width;
        output->height = height;
    }
}

static void output_done(void *data, struct wl_output *wl_output) {
    (void)data;
    output_t *output = find_output_by_wl(wl_output);
    if (output) {
        output->done = true;
        LOG("Output %d done: %s %dx%d @ %d,%d\n", 
            (int)(output - outputs), output->name, 
            output->width, output->height, output->x, output->y);
    }
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
    (void)data;
    output_t *output = find_output_by_wl(wl_output);
    if (output) output->scale = factor;
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
    (void)data;
    output_t *output = find_output_by_wl(wl_output);
    if (output && name) {
        strncpy(output->name, name, sizeof(output->name) - 1);
    }
}

static void output_description(void *data, struct wl_output *wl_output, const char *desc) {
    (void)data; (void)wl_output; (void)desc;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

/* ========== Registry Callbacks ========== */

static void registry_global(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    
    LOG("Registry: %s v%u (name=%u)\n", interface, version, name);
    
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } 
    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } 
    else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (num_outputs < MAX_OUTPUTS) {
            output_t *output = &outputs[num_outputs++];
            memset(output, 0, sizeof(*output));
            output->global_name = name;
            output->scale = 1;
            snprintf(output->name, sizeof(output->name), "output-%d", num_outputs - 1);
            
            /* Bind to at least version 4 for output name support */
            uint32_t bind_version = version < 4 ? version : 4;
            output->wl_output = wl_registry_bind(registry, name, 
                                                  &wl_output_interface, bind_version);
            wl_output_add_listener(output->wl_output, &output_listener, NULL);
        }
    } 
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(registry, name, 
                                       &zwlr_layer_shell_v1_interface, 
                                       version < 4 ? version : 4);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ========== Panel Creation ========== */

static panel_t *create_panel(output_t *output, uint32_t anchor, uint32_t width, uint32_t height) {
    panel_t *panel = calloc(1, sizeof(panel_t));
    if (!panel) return NULL;
    
    panel->anchor = anchor;
    panel->width = width;
    panel->height = height;
    
    /* Create surface */
    panel->wl_surface = wl_compositor_create_surface(wl_compositor);
    if (!panel->wl_surface) {
        ERR("Failed to create wl_surface\n");
        free(panel);
        return NULL;
    }
    
    /* Create layer surface */
    panel->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell,
        panel->wl_surface,
        output ? output->wl_output : NULL,  /* NULL = compositor chooses */
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "ringlight"
    );
    
    if (!panel->layer_surface) {
        ERR("Failed to create layer surface\n");
        wl_surface_destroy(panel->wl_surface);
        free(panel);
        return NULL;
    }
    
    /* Configure layer surface */
    zwlr_layer_surface_v1_add_listener(panel->layer_surface, &layer_surface_listener, panel);
    zwlr_layer_surface_v1_set_size(panel->layer_surface, width, height);
    zwlr_layer_surface_v1_set_anchor(panel->layer_surface, anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(panel->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(panel->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    
    /* Initial commit to trigger configure event */
    wl_surface_commit(panel->wl_surface);
    
    LOG("Created panel %ux%u anchor=0x%x\n", width, height, anchor);
    return panel;
}

static void destroy_panel(panel_t *panel) {
    if (!panel) return;
    
    destroy_panel_buffer(panel);
    
    if (panel->layer_surface) {
        zwlr_layer_surface_v1_destroy(panel->layer_surface);
    }
    if (panel->wl_surface) {
        wl_surface_destroy(panel->wl_surface);
    }
    free(panel);
}

/* ========== Config Loading ========== */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t')) *e-- = '\0';
    return s;
}

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
        
        *eq = '\0';
        char *k = trim(line);
        char *v = trim(eq + 1);
        
        if (strcmp(k, key) == 0) {
            strncpy(value, v, sizeof(value) - 1);
            fclose(f);
            return value;
        }
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
    printf("Options:\n");
    printf("  -s, --screen N|NAME  Screen index or name (default: 0)\n");
    printf("  -w, --width N        Border width in pixels (default: 80)\n");
    printf("  -c, --color RRGGBB   Color in hex (default: FFFFFF)\n");
    printf("  -b, --brightness N   Brightness 1-100 (default: 100)\n");
    printf("  -f, --fullscreen     Full screen mode\n");
    printf("  -l, --list           List available screens and exit\n");
    printf("  -v, --verbose        Verbose output\n");
    printf("  -h, --help           Show this help\n");
    printf("\nPress Ctrl+C to exit.\n");
}

int main(int argc, char *argv[]) {
    /* Load config file first */
    load_config();
    
    /* Parse command line */
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
        case 's':
            strncpy(cfg_target_name, optarg, sizeof(cfg_target_name) - 1);
            cfg_target_output = atoi(optarg);
            break;
        case 'w':
            cfg_border_width = atoi(optarg);
            if (cfg_border_width < 1) cfg_border_width = 1;
            if (cfg_border_width > 500) cfg_border_width = 500;
            break;
        case 'c':
            if (optarg[0] == '#') optarg++;
            cfg_color = strtoul(optarg, NULL, 16);
            break;
        case 'b':
            cfg_brightness = atoi(optarg);
            if (cfg_brightness < 1) cfg_brightness = 1;
            if (cfg_brightness > 100) cfg_brightness = 100;
            break;
        case 'f':
            cfg_fullscreen = true;
            break;
        case 'l':
            cfg_list_only = true;
            break;
        case 'v':
            cfg_verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    
    /* Connect to Wayland */
    wl_display = wl_display_connect(NULL);
    if (!wl_display) {
        ERR("Failed to connect to Wayland display\n");
        return 1;
    }
    
    /* Get registry and bind globals */
    wl_registry = wl_display_get_registry(wl_display);
    wl_registry_add_listener(wl_registry, &registry_listener, NULL);
    wl_display_roundtrip(wl_display);
    
    /* Wait for output info to arrive */
    wl_display_roundtrip(wl_display);
    
    /* Check required interfaces */
    if (!wl_compositor) {
        ERR("Compositor doesn't support wl_compositor\n");
        return 1;
    }
    if (!wl_shm) {
        ERR("Compositor doesn't support wl_shm\n");
        return 1;
    }
    if (!layer_shell) {
        ERR("Compositor doesn't support wlr-layer-shell-unstable-v1\n");
        ERR("Make sure you're running KDE Plasma 5.20+, Sway, or another compatible compositor\n");
        return 1;
    }
    
    /* List mode */
    if (cfg_list_only) {
        printf("Available screens:\n");
        for (int i = 0; i < num_outputs; i++) {
            printf("  %d: %s (%dx%d @ %d,%d)\n", 
                   i, outputs[i].name, 
                   outputs[i].width, outputs[i].height,
                   outputs[i].x, outputs[i].y);
        }
        wl_display_disconnect(wl_display);
        return 0;
    }
    
    if (num_outputs == 0) {
        ERR("No outputs found\n");
        return 1;
    }
    
    /* Find target output */
    output_t *target = &outputs[0];
    
    if (cfg_target_name[0]) {
        /* Try matching by name first */
        for (int i = 0; i < num_outputs; i++) {
            if (strcmp(outputs[i].name, cfg_target_name) == 0) {
                target = &outputs[i];
                break;
            }
        }
        /* Then try as index */
        if (cfg_target_output >= 0 && cfg_target_output < num_outputs) {
            target = &outputs[cfg_target_output];
        }
    }
    
    printf("%s on %s (%dx%d @ %d,%d)\n",
           cfg_fullscreen ? "Fullscreen" : "Ring",
           target->name, target->width, target->height,
           target->x, target->y);
    
    /* Create panels */
    uint32_t screen_w = target->width;
    uint32_t screen_h = target->height;
    
    if (cfg_fullscreen) {
        uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        panels[num_panels++] = create_panel(target, anchor, 0, 0);  /* 0,0 = fill anchored area */
    } else {
        /* Top */
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            0, cfg_border_width);  /* 0 width = fill horizontal */
        
        /* Bottom */
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            0, cfg_border_width);
        
        /* Left */
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            cfg_border_width, 0);  /* 0 height = fill vertical */
        
        /* Right */
        panels[num_panels++] = create_panel(target,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            cfg_border_width, 0);
    }
    
    /* Verify panels created */
    for (int i = 0; i < num_panels; i++) {
        if (!panels[i]) {
            ERR("Failed to create panel %d\n", i);
            running = 0;
            break;
        }
    }
    
    /* Setup signal handlers */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    /* Process events until all panels are configured */
    LOG("Waiting for configure events...\n");
    while (running) {
        if (wl_display_dispatch(wl_display) < 0) {
            ERR("Wayland dispatch error\n");
            break;
        }
        
        /* Check if all panels configured */
        bool all_configured = true;
        for (int i = 0; i < num_panels; i++) {
            if (panels[i] && !panels[i]->configured) {
                all_configured = false;
                break;
            }
        }
        if (all_configured) {
            LOG("All panels configured, entering main loop\n");
            break;
        }
    }
    
    /* Main event loop */
    while (running) {
        if (wl_display_dispatch(wl_display) < 0) {
            break;
        }
    }
    
    LOG("Shutting down...\n");
    
    /* Cleanup */
    for (int i = 0; i < num_panels; i++) {
        destroy_panel(panels[i]);
    }
    
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (wl_shm) wl_shm_destroy(wl_shm);
    if (wl_compositor) wl_compositor_destroy(wl_compositor);
    
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].wl_output) wl_output_destroy(outputs[i].wl_output);
    }
    
    wl_registry_destroy(wl_registry);
    wl_display_disconnect(wl_display);
    
    return 0;
}
