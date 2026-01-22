PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all clean install install-service uninstall gui monitor

all: monitor gui

monitor: ringlight-monitor

gui: build/ringlight-gui build/ringlight-overlay

ringlight-monitor: src/monitor.c
	$(CC) $(CFLAGS) -o $@ $<
	strip $@

build/ringlight-gui build/ringlight-overlay: src/gui.cpp src/overlay.c CMakeLists.txt
	@mkdir -p build
	@cd build && cmake .. && make -j$$(nproc)

clean:
	rm -rf build ringlight-monitor

install: all
	install -Dm755 ringlight-monitor $(DESTDIR)$(BINDIR)/ringlight-monitor
	install -Dm755 ringlight-monitor-wrapper $(DESTDIR)$(BINDIR)/ringlight-monitor-wrapper
	@if [ -f build/ringlight-gui ]; then install -Dm755 build/ringlight-gui $(DESTDIR)$(BINDIR)/ringlight-gui; fi
	@if [ -f build/ringlight-overlay ]; then install -Dm755 build/ringlight-overlay $(DESTDIR)$(BINDIR)/ringlight-overlay; fi
	@echo ""
	@echo "Setting CAP_NET_ADMIN capability for process monitoring..."
	-setcap cap_net_admin+ep $(DESTDIR)$(BINDIR)/ringlight-monitor 2>/dev/null || \
		echo "WARNING: Could not set capability. Run: sudo setcap cap_net_admin+ep $(BINDIR)/ringlight-monitor"

install-service:
	mkdir -p $(SYSTEMD_USER_DIR)
	install -Dm644 ringlight-monitor.service $(SYSTEMD_USER_DIR)/ringlight-monitor.service
	@echo ""
	@echo "Service installed. To enable:"
	@echo "  systemctl --user daemon-reload"
	@echo "  systemctl --user enable --now ringlight-monitor"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ringlight-monitor
	rm -f $(DESTDIR)$(BINDIR)/ringlight-monitor-wrapper
	rm -f $(DESTDIR)$(BINDIR)/ringlight-gui
	rm -f $(DESTDIR)$(BINDIR)/ringlight-overlay
	rm -f $(SYSTEMD_USER_DIR)/ringlight-monitor.service
	-systemctl --user disable ringlight-monitor 2>/dev/null
