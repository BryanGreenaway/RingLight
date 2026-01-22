# RingLight

Screen ring light for KDE Plasma 6 / Wayland. Creates bright borders around your monitor(s) for webcam illumination during video calls and face recognition (Howdy).

## Features

- White/colored screen borders with adjustable brightness and width
- Layer-shell overlay (stays above all windows including fullscreen)
- Multi-monitor support (separate process per screen)
- GUI with system tray integration
- Automatic activation modes:
  - **Process mode**: Zero-polling, event-driven detection of specific apps (howdy)
  - **Camera mode**: Polls V4L2 device for any camera activity
  - **Hybrid mode**: Both methods combined

## Building

```bash
make
sudo make install
```

**Dependencies:**
- Qt6 (Core, Gui, Widgets) - for GUI
- wayland-client, wayland-protocols - for overlay
- cmake, pkg-config

On Arch/CachyOS:
```bash
sudo pacman -S qt6-base wayland wayland-protocols cmake pkg-config
```

## Usage

### GUI
```bash
ringlight-gui
```

### Command Line
```bash
# Direct overlay on screen 0
ringlight-overlay -s 0

# Custom color and brightness
ringlight-overlay -s 0 -c FF9900 -b 80

# List screens
ringlight-overlay -l

# Monitor daemon (auto-enable with howdy)
ringlight-monitor -v

# Monitor modes
ringlight-monitor -m process -p howdy      # No polling, watches process
ringlight-monitor -m camera -i 2000        # Polls camera every 2s
ringlight-monitor -m hybrid                # Both
```

## Monitor Modes

| Mode | Polling | CPU Usage | Use Case |
|------|---------|-----------|----------|
| `process` | None | ~0% | Howdy, specific apps only |
| `camera` | Every Nms | ~0.1% | Any camera activity |
| `hybrid` | Every Nms | ~0.1% | Both methods |

For Howdy face recognition, use `process` mode - it's instant and uses no CPU when idle.

## Configuration

Settings are stored in `~/.config/ringlight/config.ini`:

```ini
[monitor]
mode=process
watch_processes=howdy
poll_interval=2000
video_device=/dev/video0

[overlay]
color=FFFFFF
brightness=100
width=80
fullscreen=false
screens=0,1
```

## Systemd Service

```bash
# Install service
make install-service

# Enable and start
systemctl --user daemon-reload
systemctl --user enable --now ringlight-monitor

# Check status
journalctl --user -u ringlight-monitor -f
```

## Architecture

```
┌─────────────────┐
│  ringlight-gui  │  Qt settings panel + tray
└────────┬────────┘
         │ spawns
         ▼
┌─────────────────┐
│ringlight-monitor│  Watches processes/camera
└────────┬────────┘
         │ spawns when active
         ▼
┌──────────────────┐  ┌──────────────────┐
│ ringlight-overlay│  │ ringlight-overlay│
│   (screen 0)     │  │   (screen 1)     │
└──────────────────┘  └──────────────────┘
```

## Troubleshooting

**Ring light not appearing:**
```bash
# Test overlay directly
ringlight-overlay -s 0 -v

# List available screens
ringlight-overlay -l
```

**Monitor not detecting howdy:**
```bash
# Run with verbose
ringlight-monitor -m process -p howdy -v

# Check netlink permissions (needs CAP_NET_ADMIN)
sudo setcap cap_net_admin+ep /usr/bin/ringlight-monitor
```

**Systemd service not working:**
```bash
# Check logs
journalctl --user -u ringlight-monitor -f

# Verify Wayland environment
echo $WAYLAND_DISPLAY
echo $XDG_RUNTIME_DIR
```

## License

MIT
