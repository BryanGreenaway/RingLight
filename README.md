# RingLight

Screen ring light for KDE Plasma 6 / Wayland. Creates bright borders around your monitor(s) for video calls and face recognition authentication (Howdy).

## Features

- White/colored screen borders with adjustable brightness and width
- **Fullscreen mode** - turn your entire monitor into a light source
- Layer-shell overlay (stays above fullscreen apps)
- **True multi-monitor support** - each screen gets its own overlay
- **Click to close** - click anywhere on the overlay to dismiss
- GUI with system tray integration
- **Event-driven automatic activation** - zero CPU when idle
- Automatic activation when webcam is in use

## Installation

### Arch Linux (AUR)

```bash
yay -S ringlight
# or
paru -S ringlight
```

### From Source

**Dependencies:**
- Qt6 (Core, Gui, Widgets)
- wayland-client
- wayland-protocols (build only)
- cmake, pkg-config (build only)

```bash
# Arch Linux
sudo pacman -S qt6-base wayland wayland-protocols cmake

# Build and install
make
sudo make install
```

## Usage

### GUI (Recommended)

```bash
ringlight-gui
```

The GUI provides:
- Screen selection for multi-monitor setups
- Color, brightness, and border width controls
- Manual on/off toggle
- Automatic webcam detection
- System tray integration

### Command Line

```bash
# Single screen overlay
ringlight-overlay              # Default screen
ringlight-overlay -s DP-1      # By screen name (recommended)
ringlight-overlay -s 0         # By index

# Multi-monitor
ringlight-overlay -s HDMI-A-1 &
ringlight-overlay -s DP-1 &

# Fullscreen mode
ringlight-overlay -f -s DP-1

# List screens
ringlight-overlay -l
```

### Automatic Activation

The monitor daemon watches for webcam usage and automatically shows the ring light:

```bash
# Enable at login
systemctl --user enable --now ringlight-monitor

# Or run directly
ringlight-monitor -v
```

## Event-Driven Mode

By default, `ringlight-monitor` uses efficient kernel events instead of polling:

- **Netlink process connector** - instant notification when Howdy starts
- **fanotify** - notification when video device is accessed

This requires capabilities that are set automatically during installation. If not available, it falls back to 1-second polling.

### Manual Capability Setup

If capabilities weren't set during installation:

```bash
sudo setcap cap_net_admin,cap_sys_admin+ep /usr/bin/ringlight-monitor
# or
sudo setcap cap_net_admin,cap_sys_admin+ep /usr/local/bin/ringlight-monitor
```

Check current capabilities:
```bash
getcap /usr/bin/ringlight-monitor
```

## Options

### ringlight-overlay

| Flag | Description |
|------|-------------|
| `-s N\|NAME` | Screen index or name (use name for reliability) |
| `-w N` | Border width in pixels (default: 80) |
| `-c RRGGBB` | Color in hex (default: FFFFFF) |
| `-b N` | Brightness 1-100 (default: 100) |
| `-f` | Fullscreen mode |
| `-l` | List available screens |
| `-v` | Verbose output |

### ringlight-monitor

| Flag | Description |
|------|-------------|
| `-d DEV` | Video device (default: /dev/video0) |
| `-p NAME` | Process to watch (default: howdy, can repeat) |
| `-c RRGGBB` | Color for overlay |
| `-b N` | Brightness 1-100 |
| `-w N` | Border width |
| `-s LIST` | Screens (comma-separated names) |
| `-f` | Fullscreen mode |
| `-v` | Verbose output |

## Configuration

Settings are stored in `~/.config/ringlight/config.ini` and managed by the GUI.

## How to Close

- **Click** anywhere on the ring light overlay
- Press **Ctrl+C** in the terminal
- Use the GUI's "Turn Off" button
- `pkill ringlight-overlay`

## Multi-Monitor Notes

Use screen **names** (like `DP-1`, `HDMI-A-1`) rather than indices for reliable targeting. Screen indices can change between reboots.

```bash
# Find your screen names
ringlight-overlay -l

# Target specific screens
ringlight-overlay -s DP-1 &
ringlight-overlay -s HDMI-A-1 &
```

## Troubleshooting

### Monitor not detecting webcam usage

1. Check if event-driven mode is working:
   ```bash
   ringlight-monitor -v
   ```
   Look for "Event-driven monitor ready" vs "Polling mode"

2. If using polling mode, grant capabilities:
   ```bash
   sudo setcap cap_net_admin,cap_sys_admin+ep $(which ringlight-monitor)
   ```

### Overlay not appearing on correct screen

Use screen names instead of indices:
```bash
ringlight-overlay -l  # List screens
ringlight-overlay -s DP-1  # Use the name
```

### High CPU usage

Ensure event-driven mode is active (see above). Polling mode uses ~1% CPU.

## Integration with Howdy

RingLight was designed to work with [Howdy](https://github.com/boltgolt/howdy) for face recognition authentication. When Howdy runs for PAM authentication (login, sudo, etc.), RingLight automatically illuminates your face for the IR camera.

Add `howdy` to the watched processes (default) in the GUI or:
```bash
ringlight-monitor -p howdy
```

## License

MIT
