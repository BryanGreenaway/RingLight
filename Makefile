# RingLight - Screen ring light for KDE Plasma 6

BUILD_DIR = build
PREFIX ?= /usr/local

.PHONY: all clean install uninstall

all:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(PREFIX) ..
	@cmake --build $(BUILD_DIR) -j$$(nproc)
	@echo ""
	@echo "Build complete:"
	@ls -lh $(BUILD_DIR)/ringlight-overlay $(BUILD_DIR)/ringlight-gui $(BUILD_DIR)/ringlight-monitor

clean:
	rm -rf $(BUILD_DIR)

install: all
	@cmake --install $(BUILD_DIR)
	@echo ""
	@echo "Installed to $(PREFIX)"
	@echo ""
	@echo "To start the GUI:"
	@echo "  ringlight-gui"
	@echo ""
	@echo "Or enable auto-start at login:"
	@echo "  systemctl --user enable ringlight-monitor"

uninstall:
	rm -f $(PREFIX)/bin/ringlight-overlay
	rm -f $(PREFIX)/bin/ringlight-gui
	rm -f $(PREFIX)/bin/ringlight-monitor
	rm -f $(PREFIX)/lib/systemd/user/ringlight-monitor.service
	rm -f $(PREFIX)/share/applications/ringlight.desktop
