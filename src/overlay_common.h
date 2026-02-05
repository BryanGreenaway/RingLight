/*
 * RingLight Overlay - Shared types and backend interface
 *
 * Copyright (C) 2024-2025 Bryan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OVERLAY_COMMON_H
#define OVERLAY_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>

/* Configuration */
typedef struct {
    int border_width;
    int brightness;
    uint32_t color;
    bool fullscreen;
    char target_name[64];
    bool list_only;
    bool verbose;
} config_t;

/* Logging */
#define LOG(cfg, ...) do { if ((cfg)->verbose) fprintf(stderr, "[ringlight] " __VA_ARGS__); } while(0)
#define ERR(...) fprintf(stderr, "[ringlight] " __VA_ARGS__)

/* Shared state */
extern volatile sig_atomic_t running;

/* Color calculation (ARGB8888) */
static inline uint32_t calculate_pixel(uint32_t color, int brightness) {
    uint32_t r = ((color >> 16) & 0xFF) * brightness / 100;
    uint32_t g = ((color >> 8) & 0xFF) * brightness / 100;
    uint32_t b = (color & 0xFF) * brightness / 100;
    return (0xFFu << 24) | (r << 16) | (g << 8) | b;
}

/* Backend functions */
#ifdef HAVE_WAYLAND
int wayland_list_screens(config_t *cfg);
int wayland_run(config_t *cfg);
#endif

#ifdef HAVE_X11
int x11_list_screens(config_t *cfg);
int x11_run(config_t *cfg);
#endif

#endif /* OVERLAY_COMMON_H */
