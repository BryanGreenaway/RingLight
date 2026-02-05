/*
 * RingLight Overlay - Screen ring light for Wayland and X11
 *
 * Supports both Wayland (wlr-layer-shell) and X11 (Xlib) backends.
 * Click anywhere on the overlay to close.
 *
 * Copyright (C) 2024-2025 Bryan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <pwd.h>

#include "overlay_common.h"

volatile sig_atomic_t running = 1;

static config_t cfg = {
    .border_width = 80,
    .brightness = 100,
    .color = 0xFFFFFF,
    .fullscreen = false,
    .target_name = "",
    .list_only = false,
    .verbose = false,
};

typedef enum {
    BACKEND_AUTO,
    BACKEND_WAYLAND,
    BACKEND_X11,
} backend_t;

static backend_t selected_backend = BACKEND_AUTO;

/* Config file parsing */
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

        char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        char *kend = eq;
        while (kend > k && (*(kend-1) == ' ' || *(kend-1) == '\t')) kend--;

        if ((size_t)(kend - k) != keylen || strncmp(k, key, keylen) != 0) continue;

        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *vend = v + strlen(v);
        while (vend > v && (*(vend-1) == '\n' || *(vend-1) == '\r' || *(vend-1) == ' ')) vend--;
        *vend = '\0';

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
        cfg.border_width = atoi(v);
        if (cfg.border_width < 1) cfg.border_width = 1;
        if (cfg.border_width > 500) cfg.border_width = 500;
    }
    if ((v = get_config_value(path, "brightness"))) {
        cfg.brightness = atoi(v);
        if (cfg.brightness < 1) cfg.brightness = 1;
        if (cfg.brightness > 100) cfg.brightness = 100;
    }
    if ((v = get_config_value(path, "color"))) {
        if (v[0] == '#') v++;
        cfg.color = strtoul(v, NULL, 16);
    }
    if ((v = get_config_value(path, "fullscreen"))) {
        cfg.fullscreen = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
}

static void sig_handler(int sig) { (void)sig; running = 0; }

static void print_usage(const char *prog) {
    printf("ringlight-overlay - Screen ring light\n\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("  -s, --screen N|NAME  Screen index or name\n");
    printf("  -w, --width N        Border width in pixels (default: 80)\n");
    printf("  -c, --color RRGGBB   Color in hex (default: FFFFFF)\n");
    printf("  -b, --brightness N   Brightness 1-100 (default: 100)\n");
    printf("  -f, --fullscreen     Full screen mode\n");
    printf("  -B, --backend TYPE   Backend: auto, wayland, x11 (default: auto)\n");
    printf("  -l, --list           List screens and exit\n");
    printf("  -v, --verbose        Verbose output\n");
    printf("  -h, --help           Show this help\n");
    printf("\nClick on the overlay to close.\n");
}

int main(int argc, char *argv[]) {
    load_config();

    static struct option long_opts[] = {
        {"screen",     required_argument, 0, 's'},
        {"width",      required_argument, 0, 'w'},
        {"color",      required_argument, 0, 'c'},
        {"brightness", required_argument, 0, 'b'},
        {"fullscreen", no_argument,       0, 'f'},
        {"backend",    required_argument, 0, 'B'},
        {"list",       no_argument,       0, 'l'},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:w:c:b:fB:lvh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': strncpy(cfg.target_name, optarg, sizeof(cfg.target_name) - 1); break;
        case 'w':
            cfg.border_width = atoi(optarg);
            if (cfg.border_width < 1) cfg.border_width = 1;
            if (cfg.border_width > 500) cfg.border_width = 500;
            break;
        case 'c':
            if (optarg[0] == '#') optarg++;
            cfg.color = strtoul(optarg, NULL, 16);
            break;
        case 'b':
            cfg.brightness = atoi(optarg);
            if (cfg.brightness < 1) cfg.brightness = 1;
            if (cfg.brightness > 100) cfg.brightness = 100;
            break;
        case 'f': cfg.fullscreen = true; break;
        case 'B':
            if (strcmp(optarg, "wayland") == 0) selected_backend = BACKEND_WAYLAND;
            else if (strcmp(optarg, "x11") == 0) selected_backend = BACKEND_X11;
            else if (strcmp(optarg, "auto") == 0) selected_backend = BACKEND_AUTO;
            else { ERR("Unknown backend: %s\n", optarg); return 1; }
            break;
        case 'l': cfg.list_only = true; break;
        case 'v': cfg.verbose = true; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Auto-detect backend */
    if (selected_backend == BACKEND_AUTO) {
#ifdef HAVE_WAYLAND
        if (getenv("WAYLAND_DISPLAY"))
            selected_backend = BACKEND_WAYLAND;
        else
#endif
#ifdef HAVE_X11
            selected_backend = BACKEND_X11;
#else
        {
            ERR("No suitable backend available\n");
            return 1;
        }
#endif
    }

    /* Dispatch to selected backend */
    switch (selected_backend) {
#ifdef HAVE_WAYLAND
    case BACKEND_WAYLAND:
        LOG(&cfg, "Using Wayland backend\n");
        return cfg.list_only ? wayland_list_screens(&cfg) : wayland_run(&cfg);
#endif
#ifdef HAVE_X11
    case BACKEND_X11:
        LOG(&cfg, "Using X11 backend\n");
        return cfg.list_only ? x11_list_screens(&cfg) : x11_run(&cfg);
#endif
    default:
        ERR("Selected backend not compiled in\n");
        return 1;
    }
}
