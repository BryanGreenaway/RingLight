/*
 * RingLight Overlay - Pure Wayland layer-shell overlay
 * 
 * Uses wlr-layer-shell protocol directly, no Qt window management.
 * Each process handles ONE screen.
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

#include <wayland-client.h>
#include "xdg-shell-client.h"
#include "wlr-layer-shell-unstable-v1-client.h"

// Shared memory helpers
#include <sys/mman.h>
#include <time.h>

static volatile sig_atomic_t running = 1;

// Wayland globals
static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_shm *shm = NULL;
static struct zwlr_layer_shell_v1 *layer_shell = NULL;

// Output tracking
#define MAX_OUTPUTS 16
static struct {
    struct wl_output *output;
    char name[64];
    int32_t x, y;
    int32_t width, height;
    int32_t scale;
    bool done;
} outputs[MAX_OUTPUTS];
static int output_count = 0;

// Panel state
struct panel {
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;
    void *shm_data;
    int width, height;
    bool configured;
};

// Config
static int border_width = 80;
static int brightness = 100;
static uint32_t color_r = 255, color_g = 255, color_b = 255;
static bool fullscreen_mode = false;
static int target_output = 0;
static char target_output_name[64] = "";
static bool list_outputs = false;

// Signal handler
static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

// Create anonymous file for shared memory
static int create_shm_file(size_t size) {
    char name[64];
    snprintf(name, sizeof(name), "/ringlight-%d", getpid());
    
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    
    shm_unlink(name);
    
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    
    return fd;
}

// Create a buffer with solid color
static struct wl_buffer *create_buffer(int width, int height, void **data_out) {
    int stride = width * 4;
    int size = stride * height;
    
    int fd = create_shm_file(size);
    if (fd < 0) {
        fprintf(stderr, "Failed to create shm file\n");
        return NULL;
    }
    
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, 
                                                          stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    
    // Fill with color (ARGB format)
    uint32_t *pixels = data;
    uint32_t pixel = (255 << 24) | (color_r << 16) | (color_g << 8) | color_b;
    for (int i = 0; i < width * height; i++) {
        pixels[i] = pixel;
    }
    
    *data_out = data;
    return buffer;
}

// Layer surface listener
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    struct panel *p = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    
    if (width > 0 && height > 0 && (p->width != (int)width || p->height != (int)height)) {
        p->width = width;
        p->height = height;
        
        // Recreate buffer with new size
        if (p->buffer) {
            wl_buffer_destroy(p->buffer);
            if (p->shm_data) {
                munmap(p->shm_data, p->width * p->height * 4);
            }
        }
        
        p->buffer = create_buffer(width, height, &p->shm_data);
    }
    
    if (p->buffer) {
        wl_surface_attach(p->surface, p->buffer, 0, 0);
        wl_surface_damage_buffer(p->surface, 0, 0, p->width, p->height);
        wl_surface_commit(p->surface);
    }
    
    p->configured = true;
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)data;
    (void)surface;
    running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

// Create a panel on specific output
static struct panel *create_panel(struct wl_output *output, uint32_t anchor, 
                                   int width, int height) {
    struct panel *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    
    p->surface = wl_compositor_create_surface(compositor);
    if (!p->surface) {
        free(p);
        return NULL;
    }
    
    p->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell,
        p->surface,
        output,  // Specific output!
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "ringlight"
    );
    
    if (!p->layer_surface) {
        wl_surface_destroy(p->surface);
        free(p);
        return NULL;
    }
    
    zwlr_layer_surface_v1_add_listener(p->layer_surface, &layer_surface_listener, p);
    zwlr_layer_surface_v1_set_size(p->layer_surface, width, height);
    zwlr_layer_surface_v1_set_anchor(p->layer_surface, anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(p->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(p->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    
    p->width = width;
    p->height = height;
    
    // Initial commit to get configure event
    wl_surface_commit(p->surface);
    
    return p;
}

static void destroy_panel(struct panel *p) {
    if (!p) return;
    if (p->layer_surface) zwlr_layer_surface_v1_destroy(p->layer_surface);
    if (p->surface) wl_surface_destroy(p->surface);
    if (p->buffer) wl_buffer_destroy(p->buffer);
    if (p->shm_data) munmap(p->shm_data, p->width * p->height * 4);
    free(p);
}

// Output listener
static void output_geometry(void *data, struct wl_output *output,
                           int32_t x, int32_t y, int32_t pw, int32_t ph,
                           int32_t subpixel, const char *make, const char *model,
                           int32_t transform) {
    (void)pw; (void)ph; (void)subpixel; (void)make; (void)model; (void)transform;
    
    for (int i = 0; i < output_count; i++) {
        if (outputs[i].output == output) {
            outputs[i].x = x;
            outputs[i].y = y;
            break;
        }
    }
}

static void output_mode(void *data, struct wl_output *output,
                        uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    (void)refresh;
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    
    for (int i = 0; i < output_count; i++) {
        if (outputs[i].output == output) {
            outputs[i].width = width;
            outputs[i].height = height;
            break;
        }
    }
}

static void output_done(void *data, struct wl_output *output) {
    for (int i = 0; i < output_count; i++) {
        if (outputs[i].output == output) {
            outputs[i].done = true;
            break;
        }
    }
}

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
    for (int i = 0; i < output_count; i++) {
        if (outputs[i].output == output) {
            outputs[i].scale = factor;
            break;
        }
    }
}

static void output_name(void *data, struct wl_output *output, const char *name) {
    for (int i = 0; i < output_count; i++) {
        if (outputs[i].output == output) {
            strncpy(outputs[i].name, name, sizeof(outputs[i].name) - 1);
            break;
        }
    }
}

static void output_description(void *data, struct wl_output *output, const char *desc) {
    (void)data; (void)output; (void)desc;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

// Registry listener
static void registry_global(void *data, struct wl_registry *reg,
                           uint32_t name, const char *interface, uint32_t version) {
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (output_count < MAX_OUTPUTS) {
            outputs[output_count].output = wl_registry_bind(reg, name, 
                &wl_output_interface, version >= 4 ? 4 : version);
            outputs[output_count].scale = 1;
            wl_output_add_listener(outputs[output_count].output, &output_listener, NULL);
            output_count++;
        }
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
                                       version >= 4 ? 4 : version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

// Load config file
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s) - 1;
    while (e > s && (*e == '\n' || *e == '\r' || *e == ' ')) *e-- = '\0';
    return s;
}

static char *get_config(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    
    static char val[256];
    char line[512];
    size_t klen = strlen(key);
    
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq || (size_t)(eq - line) != klen || strncmp(line, key, klen) != 0) continue;
        strncpy(val, trim(eq + 1), sizeof(val) - 1);
        fclose(f);
        return val;
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
    if ((v = get_config(path, "width"))) {
        border_width = atoi(v);
        if (border_width < 1) border_width = 1;
        if (border_width > 500) border_width = 500;
    }
    if ((v = get_config(path, "brightness"))) {
        brightness = atoi(v);
        if (brightness < 1) brightness = 1;
        if (brightness > 100) brightness = 100;
    }
    if ((v = get_config(path, "color"))) {
        if (v[0] == '#') v++;
        unsigned int c = strtoul(v, NULL, 16);
        color_r = ((c >> 16) & 0xFF) * brightness / 100;
        color_g = ((c >> 8) & 0xFF) * brightness / 100;
        color_b = (c & 0xFF) * brightness / 100;
    }
    if ((v = get_config(path, "fullscreen"))) {
        fullscreen_mode = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
}

static void parse_color(const char *hex) {
    if (hex[0] == '#') hex++;
    unsigned int c = strtoul(hex, NULL, 16);
    color_r = ((c >> 16) & 0xFF) * brightness / 100;
    color_g = ((c >> 8) & 0xFF) * brightness / 100;
    color_b = (c & 0xFF) * brightness / 100;
}

static void print_usage(const char *prog) {
    printf("ringlight-overlay - Screen ring light for Wayland\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -s, --screen N     Screen index or name\n");
    printf("  -w, --width N      Border width in pixels (default: 80)\n");
    printf("  -c, --color HEX    Color as RRGGBB (default: FFFFFF)\n");
    printf("  -b, --brightness N Brightness 1-100 (default: 100)\n");
    printf("  -f, --fullscreen   Fullscreen mode (whole screen lights up)\n");
    printf("  -l, --list         List available screens\n");
    printf("  -h, --help         Show this help\n");
}

int main(int argc, char *argv[]) {
    // Load config first
    load_config();
    
    static struct option opts[] = {
        {"screen", 1, 0, 's'},
        {"width", 1, 0, 'w'},
        {"color", 1, 0, 'c'},
        {"brightness", 1, 0, 'b'},
        {"fullscreen", 0, 0, 'f'},
        {"list", 0, 0, 'l'},
        {"help", 0, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "s:w:c:b:flh", opts, NULL)) != -1) {
        switch (opt) {
        case 's':
            // Try as number first
            target_output = atoi(optarg);
            strncpy(target_output_name, optarg, sizeof(target_output_name) - 1);
            break;
        case 'w':
            border_width = atoi(optarg);
            if (border_width < 1) border_width = 1;
            if (border_width > 500) border_width = 500;
            break;
        case 'c':
            parse_color(optarg);
            break;
        case 'b':
            brightness = atoi(optarg);
            if (brightness < 1) brightness = 1;
            if (brightness > 100) brightness = 100;
            // Reapply to color
            color_r = color_r * brightness / 100;
            color_g = color_g * brightness / 100;
            color_b = color_b * brightness / 100;
            break;
        case 'f':
            fullscreen_mode = true;
            break;
        case 'l':
            list_outputs = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Connect to Wayland
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return 1;
    }
    
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    
    // Wait for output info
    wl_display_roundtrip(display);
    
    if (!compositor || !shm) {
        fprintf(stderr, "Missing required Wayland interfaces\n");
        return 1;
    }
    
    if (!layer_shell) {
        fprintf(stderr, "Compositor doesn't support wlr-layer-shell\n");
        return 1;
    }
    
    // List outputs mode
    if (list_outputs) {
        printf("Available outputs:\n");
        for (int i = 0; i < output_count; i++) {
            printf("  %d: %s (%dx%d @ %d,%d)\n", i, 
                   outputs[i].name[0] ? outputs[i].name : "(unnamed)",
                   outputs[i].width, outputs[i].height,
                   outputs[i].x, outputs[i].y);
        }
        wl_display_disconnect(display);
        return 0;
    }
    
    if (output_count == 0) {
        fprintf(stderr, "No outputs found\n");
        return 1;
    }
    
    // Find target output
    int target_idx = 0;
    if (target_output_name[0]) {
        // Try to match by name first
        for (int i = 0; i < output_count; i++) {
            if (strcmp(outputs[i].name, target_output_name) == 0) {
                target_idx = i;
                break;
            }
        }
        // Otherwise use as index
        int idx = atoi(target_output_name);
        if (idx >= 0 && idx < output_count) {
            target_idx = idx;
        }
    }
    
    struct wl_output *output = outputs[target_idx].output;
    int screen_w = outputs[target_idx].width;
    int screen_h = outputs[target_idx].height;
    
    printf("%s on %s (%dx%d @ %d,%d)\n",
           fullscreen_mode ? "Fullscreen" : "Ring",
           outputs[target_idx].name[0] ? outputs[target_idx].name : "(unnamed)",
           screen_w, screen_h,
           outputs[target_idx].x, outputs[target_idx].y);
    
    // Create panels
    struct panel *panels[5] = {0};
    int panel_count = 0;
    
    if (fullscreen_mode) {
        // Single fullscreen panel
        uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        panels[panel_count++] = create_panel(output, anchor, screen_w, screen_h);
    } else {
        // Four edge panels
        // Top
        panels[panel_count++] = create_panel(output,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            screen_w, border_width);
        
        // Bottom
        panels[panel_count++] = create_panel(output,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
            screen_w, border_width);
        
        // Left
        panels[panel_count++] = create_panel(output,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            border_width, screen_h);
        
        // Right
        panels[panel_count++] = create_panel(output,
            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
            border_width, screen_h);
    }
    
    // Setup signal handlers
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    // Main loop
    while (running && wl_display_dispatch(display) != -1) {
        // Just process events
    }
    
    // Cleanup
    for (int i = 0; i < panel_count; i++) {
        destroy_panel(panels[i]);
    }
    
    if (layer_shell) zwlr_layer_shell_v1_destroy(layer_shell);
    if (shm) wl_shm_destroy(shm);
    if (compositor) wl_compositor_destroy(compositor);
    for (int i = 0; i < output_count; i++) {
        wl_output_destroy(outputs[i].output);
    }
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    
    return 0;
}
