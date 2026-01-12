# RingLight

Screen ring light for KDE Plasma 6 / Wayland. Creates bright borders around your monitor(s) for video calls and face recognition (Howdy).

## Features

- White/colored screen borders with adjustable brightness and width
- **Fullscreen mode** - turn your entire monitor into a light source
- Layer-shell overlay (stays above fullscreen apps)
- **True multi-monitor support** - each screen can have its own overlay
- **Click to close** - click anywhere on the overlay to dismiss it
- GUI with system tray integration
- Automatic activation on webcam use

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

## Usage

```bash
# GUI (recommended)
ringlight-gui

# Single screen overlay
ringlight-overlay              # Default screen
ringlight-overlay -s 0         # Screen 0 (by index)
ringlight-overlay -s DP-1      # Screen by name (recommended)

# Multi-monitor: spawn separate processes
ringlight-overlay -s HDMI-A-1 &
ringlight-overlay -s DP-1 &

# Fullscreen mode
ringlight-overlay -f -s DP-1

# List available screens
ringlight-overlay -l
```

## Options

| Flag | Description |
|------|-------------|
| `-s N\|NAME` | Screen index or name (use name for reliability) |
| `-w N` | Border width in pixels (default: 80) |
| `-c HEX` | Color as RRGGBB (default: FFFFFF) |
| `-b N` | Brightness 1-100 (default: 100) |
| `-f` | Fullscreen mode |
| `-l` | List available screens |
| `-v` | Verbose output |

## How to Close

- **Click** anywhere on the ring light overlay
- Press **Ctrl+C** in the terminal
- Use the GUI's "Turn Off" button
- Run `pkill ringlight-overlay`

## Multi-Monitor Notes

Use screen **names** (like `DP-1`, `HDMI-A-1`) rather than indices for reliable targeting. Screen indices can change between reboots.

```bash
# Find your screen names
ringlight-overlay -l

# Target specific screens by name
ringlight-overlay -s DP-1 &
ringlight-overlay -s HDMI-A-1 &
```

## License

MIT
