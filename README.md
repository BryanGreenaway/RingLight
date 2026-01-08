# RingLight 🔆

Lightweight screen ring light for **KDE Plasma 6** on Wayland. Illuminates your face during video calls by creating white border overlays around your screen(s).

## Features

- **Plasma 6 native**: Uses `layer-shell-qt` for proper Wayland overlay support
- **Lightweight**: Small Qt6 binary, Qt libs already loaded by Plasma
- **Auto-activation**: Detects webcam/Howdy usage automatically
- **Click-through**: Overlays don't interfere with mouse input
- **Multi-monitor**: Works on all screens or specific ones

## Requirements

- KDE Plasma 6 on Wayland
- `layer-shell-qt` (included with Plasma 6)
- `qt6-base`
- `cmake`

## Build & Install

```bash
# Install build dependencies (CachyOS/Arch)
sudo pacman -S cmake qt6-base layer-shell-qt

# Build
make

# Install
sudo make install

# Enable auto-start for Howdy
systemctl --user enable --now ringlight-monitor
```

## Usage

### Manual

```bash
ringlight                    # All screens (default)
ringlight -s 0               # Screen 0 only
ringlight -s 1               # Screen 1 only
ringlight -s all             # All screens (explicit)
ringlight -w 100             # 100px border width
ringlight -b 70              # 70% brightness
ringlight -c FF9900          # Orange color
ringlight -l                 # List screens with their numbers
```

### Automatic (Howdy Integration)

The monitor daemon watches for `howdy` process and webcam usage:

```bash
# Run directly with logging
ringlight-monitor -v

# Or via systemd (recommended)
systemctl --user start ringlight-monitor
journalctl --user -u ringlight-monitor -f
```

**Monitor options:**
- `-d /dev/video0` - Video device to monitor
- `-p howdy` - Process to watch (repeatable)
- `-a "-s 0 -w 100"` - Args to pass to ringlight (e.g., specific screen)
- `-i 150` - Poll interval in ms (default: 150)

**Example: Ring light only on your webcam monitor:**
```bash
# Find which screen has your webcam
ringlight -l

# Configure monitor to use only that screen
ringlight-monitor -v -a "-s 1"
```

## How It Works

1. `ringlight-monitor` polls every 150ms checking:
   - Is `howdy` process running?
   - Is the video device open?
   - Is V4L2 streaming?

2. When activity detected → spawns `ringlight`
3. When activity stops → kills `ringlight`

The ring light appears before Howdy scans, illuminating your face for IR recognition.

## Troubleshooting

**Panels don't appear on some monitors**
- Run `ringlight -l` to see all detected screens
- Try targeting a specific screen: `ringlight -s 0`

**Panels appear garbled or wrong size**
- This can happen with mixed-DPI setups
- Try targeting screens individually

**Monitor not detecting Howdy**
```bash
# Test manually
ringlight-monitor -v

# Check your video device
v4l2-ctl --list-devices
# Use correct device:
ringlight-monitor -d /dev/video2
```

**Permission denied on /dev/video***
```bash
sudo usermod -aG video $USER
# Log out and back in
```

## Configuration

Edit the systemd service to persist settings:
```bash
systemctl --user edit ringlight-monitor
```

Add:
```ini
[Service]
ExecStart=
ExecStart=/usr/local/bin/ringlight-monitor -v -a "-s 0 -w 100 -b 80"
```

## License

MIT
