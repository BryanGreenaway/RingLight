# RingLight

Screen ring light for KDE Plasma 6 / Wayland. Creates bright borders around your monitor(s) for video calls and face recognition (Howdy).

## Features

- White/colored screen borders with adjustable brightness and width
- Layer-shell overlay (stays above fullscreen apps)
- Multi-monitor support (one process per screen)
- GUI with system tray integration
- Automatic activation on webcam use

## Building

```bash
make
sudo make install
```

**Dependencies:** Qt6 (Core, Gui, Widgets), layer-shell-qt, cmake

## Usage

```bash
# GUI (recommended)
ringlight-gui

# Single screen overlay
ringlight-overlay              # Default screen
ringlight-overlay -s 0         # Screen 0
ringlight-overlay -s DP-1      # By name

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
| `-w N` | Border width in pixels (default: 80) |
| `-c HEX` | Color as RRGGBB (default: FFFFFF) |
| `-b N` | Brightness 1-100 (default: 100) |
| `-l` | List available screens |

## Multi-Monitor Notes

Due to Wayland/layer-shell-qt limitations, each overlay process can only target one screen. The GUI and monitor daemon handle this automatically by spawning separate processes for each selected screen.

## License

MIT
