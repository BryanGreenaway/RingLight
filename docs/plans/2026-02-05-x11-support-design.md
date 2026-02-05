# X11 Backend Support for RingLight Overlay

## Summary

Add X11 support to ringlight-overlay so it works on both Wayland and X11 systems. Single binary with runtime auto-detection and compile-time feature flags.

## Key Decisions

- **X11 library**: Xlib (simple, sufficient for colored rectangles)
- **Multi-monitor**: XRandR (modern, provides monitor names)
- **Window type**: Override-redirect (matches Wayland layer-shell overlay behavior)
- **Detection**: Runtime auto-detect (`WAYLAND_DISPLAY` env var) with `--backend` CLI override
- **Compile-time**: `ENABLE_WAYLAND` and `ENABLE_X11` CMake options, both ON by default
- **Code organization**: Separate source files per backend

## File Structure

```
src/
  overlay_common.h    - shared types, config struct, backend interface
  overlay.c           - main(), config loading, arg parsing, backend dispatch
  overlay_wayland.c   - Wayland backend (extracted from current overlay.c)
  overlay_x11.c       - X11 backend (new)
```

## Backend Interface

Each backend implements:
- `backend_list_screens(config_t *cfg)` — enumerate and print screens
- `backend_run(config_t *cfg)` — create panels and run event loop

## X11 Backend Behavior

- XRandR for screen enumeration (name, position, dimensions)
- 4 override-redirect windows for ring mode, 1 for fullscreen
- `XAllocColor` for color handling
- `poll()` + `XPending()` event loop (same pattern as Wayland backend)
- `ButtonPressMask` for click-to-dismiss
- SIGTERM/SIGINT handled via shared `running` flag

## Backend Selection (auto mode)

1. If `WAYLAND_DISPLAY` is set and Wayland compiled in → Wayland
2. Else if X11 compiled in → X11
3. Else → error
