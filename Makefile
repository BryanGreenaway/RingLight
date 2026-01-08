# RingLight - Screen ring light for KDE Plasma 6
# Convenience Makefile wrapper for CMake

BUILD_DIR = build
PREFIX ?= /usr/local

.PHONY: all clean install uninstall

all:
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(PREFIX) ..
	@cmake --build $(BUILD_DIR) -j$$(nproc)
	@echo ""
	@echo "Built successfully. Binary sizes:"
	@ls -lh $(BUILD_DIR)/ringlight $(BUILD_DIR)/ringlight-monitor

clean:
	rm -rf $(BUILD_DIR)

install: all
	@cmake --install $(BUILD_DIR)
	@echo ""
	@echo "Installed. Enable auto-start with:"
	@echo "  systemctl --user enable --now ringlight-monitor"

uninstall:
	rm -f $(PREFIX)/bin/ringlight $(PREFIX)/bin/ringlight-monitor
	rm -f $(PREFIX)/lib/systemd/user/ringlight-monitor.service
