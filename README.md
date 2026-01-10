# RingLight

Screen ring light for KDE Plasma 6 / Wayland. Creates bright borders around your monitor(s) for video calls and face recognition (Howdy).

## Features

- White/colored screen borders with adjustable brightness and width
- **Fullscreen mode** - turn your entire monitor into a light source
- Layer-shell overlay (stays above fullscreen apps)
- **True multi-monitor support** - each screen can have its own overlay
- GUI with system tray integration
- Automatic activation on webcam use
- Settings persist between sessions

## Building

```bash
make
sudo make install
```

**Dependencies:**
- Qt6 (Core, Gui, Widgets) - for the GUI only
- wayland-client
- wayland-protocols (build only)
- wayland-scanner (build only)
- cmake, pkg-config (build only)

On Arch Linux:
```bash
sudo pacman -S qt6-base wayland wayland-protocols cmake
```

On Fedora:
```bash
sudo dnf install qt6-qtbase-devel wayland-devel wayland-protocols-devel cmake
```

On Ubuntu/Debian:
```bash
sudo apt install qt6-base-dev libwayland-dev wayland-protocols cmake pkg-config
```

## Usage

```bash
# GUI (recommended)
ringlight-gui

# Single screen overlay
ringlight-overlay              # Default screen, uses saved settings
ringlight-overlay -s 0         # Screen 0
ringlight-overlay -s DP-1      # By name

# Fullscreen mode (whole screen lights up)
ringlight-overlay -f
ringlight-overlay -f -s 1      # Fullscreen on screen 1

# Multi-monitor: spawn separate processes
ringlight-overlay -s 0 &
ringlight-overlay -s 1 &

# With options
ringlight-overlay -s 0 -c FF9900 -b 80 -w 100

# Monitor daemon (auto-enable on webcam)
ringlight-monitor -v
```

## Options

| Flag | Description |
|------|-------------|
| `-s N` | Screen index or name |
| `-w N` | Border width in pixels (default: 80, or from config) |
| `-c HEX` | Color as RRGGBB (default: FFFFFF, or from config) |
| `-b N` | Brightness 1-100 (default: 100, or from config) |
| `-f` | Fullscreen mode (entire screen lights up) |
| `-l` | List available screens with wl_output info |

## How Multi-Monitor Works

This version uses the wlr-layer-shell Wayland protocol directly with pure C (no Qt for the overlay). When you run `ringlight-overlay -s 1`, it:

1. Connects directly to Wayland and enumerates all outputs
2. Creates `wl_surface` objects manually (no Qt window management)
3. Passes the specific `wl_output` to `zwlr_layer_shell_v1_get_layer_surface()`
4. Uses shared memory buffers for rendering solid colors

This avoids the conflict where Qt assigns `xdg_toplevel` role to windows before we can assign `layer_surface` role.

The GUI and monitor daemon spawn separate overlay processes for each selected screen.

## Config File

Settings are saved to `~/.config/ringlight/config.ini` and are automatically loaded by both the overlay and monitor daemon.

## Troubleshooting

**Overlay only appears on one screen:**
- Run `ringlight-overlay -l` to verify screens are detected
- Make sure you're spawning separate processes: `ringlight-overlay -s 0 & ringlight-overlay -s 1 &`

**"Compositor doesn't support wlr-layer-shell":**
- KDE Plasma 5.20+ supports this protocol
- Other compatible compositors: Sway, Hyprland, wlroots-based compositors

**To close the overlay:**
- Press Ctrl+C in the terminal, or
- Kill the process: `pkill ringlight-overlay`
- The GUI's "Turn Off" button also stops overlays

## License

MIT
