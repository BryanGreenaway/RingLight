PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
SYSTEMD_USER_DIR ?= $(PREFIX)/lib/systemd/user

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all clean install install-user uninstall gui monitor

all: monitor gui

monitor: ringlight-monitor

gui: build/ringlight-gui build/ringlight-overlay

ringlight-monitor: src/monitor.c
	$(CC) $(CFLAGS) -o $@ $<
	strip $@

build/ringlight-gui build/ringlight-overlay: src/gui.cpp src/overlay.c CMakeLists.txt
	@mkdir -p build
	@cd build && cmake -DCMAKE_INSTALL_PREFIX=$(PREFIX) .. && make -j$$(nproc)

clean:
	rm -rf build ringlight-monitor

install: all
	install -Dm755 ringlight-monitor $(DESTDIR)$(BINDIR)/ringlight-monitor
	install -Dm755 ringlight-monitor-wrapper $(DESTDIR)$(BINDIR)/ringlight-monitor-wrapper
	install -Dm755 build/ringlight-gui $(DESTDIR)$(BINDIR)/ringlight-gui
	install -Dm755 build/ringlight-overlay $(DESTDIR)$(BINDIR)/ringlight-overlay
	install -Dm644 ringlight-monitor.service $(DESTDIR)$(SYSTEMD_USER_DIR)/ringlight-monitor.service
	install -Dm644 resources/ringlight.desktop $(DESTDIR)$(PREFIX)/share/applications/ringlight.desktop
	@echo ""
	@echo "Setting CAP_NET_ADMIN capability for process monitoring..."
	@setcap cap_net_admin+ep $(DESTDIR)$(BINDIR)/ringlight-monitor 2>/dev/null || \
		echo "Note: Could not set capability. Run: sudo setcap cap_net_admin+ep $(BINDIR)/ringlight-monitor"
	@echo ""
	@echo "Enable auto-start: systemctl --user enable ringlight-monitor"
	@echo "Start now:         systemctl --user start ringlight-monitor"

# Install to user home directory (no sudo needed except for setcap)
install-user: all
	install -Dm755 ringlight-monitor $(HOME)/.local/bin/ringlight-monitor
	install -Dm755 ringlight-monitor-wrapper $(HOME)/.local/bin/ringlight-monitor-wrapper
	install -Dm755 build/ringlight-gui $(HOME)/.local/bin/ringlight-gui
	install -Dm755 build/ringlight-overlay $(HOME)/.local/bin/ringlight-overlay
	mkdir -p $(HOME)/.config/systemd/user
	sed 's|/usr/bin|$(HOME)/.local/bin|g' ringlight-monitor.service > $(HOME)/.config/systemd/user/ringlight-monitor.service
	@echo ""
	@echo "Set capability (requires sudo):"
	@echo "  sudo setcap cap_net_admin+ep $(HOME)/.local/bin/ringlight-monitor"
	@echo ""
	@echo "Then enable:"
	@echo "  systemctl --user daemon-reload"
	@echo "  systemctl --user enable --now ringlight-monitor"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ringlight-monitor
	rm -f $(DESTDIR)$(BINDIR)/ringlight-monitor-wrapper
	rm -f $(DESTDIR)$(BINDIR)/ringlight-gui
	rm -f $(DESTDIR)$(BINDIR)/ringlight-overlay
	rm -f $(DESTDIR)$(SYSTEMD_USER_DIR)/ringlight-monitor.service
	rm -f $(DESTDIR)$(PREFIX)/share/applications/ringlight.desktop
