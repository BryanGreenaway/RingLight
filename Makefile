BUILD_DIR = build
PREFIX ?= /usr/local

.PHONY: all clean install uninstall setcap

all:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(PREFIX) ..
	@cmake --build $(BUILD_DIR) -j$$(nproc)

clean:
	rm -rf $(BUILD_DIR)

install: all
	@cmake --install $(BUILD_DIR)
	@echo ""
	@echo "Setting capabilities for event-driven monitoring..."
	@if setcap cap_net_admin,cap_sys_admin+ep $(PREFIX)/bin/ringlight-monitor 2>/dev/null; then \
		echo "  Capabilities set (zero-CPU event mode enabled)"; \
	else \
		echo "  Note: Could not set capabilities (need sudo for setcap)"; \
		echo "  Run: sudo make setcap"; \
		echo "  Or ringlight-monitor will use polling fallback"; \
	fi
	@echo ""
	@echo "Installation complete!"
	@echo "  Start GUI:        ringlight-gui"
	@echo "  Auto-start:       systemctl --user enable --now ringlight-monitor"

setcap:
	setcap cap_net_admin+ep $(PREFIX)/bin/ringlight-monitor
	@echo "Capabilities set successfully"

uninstall:
	rm -f $(PREFIX)/bin/ringlight-overlay $(PREFIX)/bin/ringlight-gui $(PREFIX)/bin/ringlight-monitor
	rm -f $(PREFIX)/lib/systemd/user/ringlight-monitor.service
	rm -f $(PREFIX)/share/applications/ringlight.desktop
