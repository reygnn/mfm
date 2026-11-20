# mfm - minimal file manager for Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wayland-client wayland-cursor xkbcommon cairo pangocairo

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
XDG_DECORATION = $(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
DATA_CONTROL = $(WAYLAND_PROTOCOLS)/staging/ext-data-control/ext-data-control-v1.xml
VIEWPORTER = $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml

PROTO_H = build/xdg-shell-client-protocol.h \
	build/xdg-decoration-unstable-v1-client-protocol.h \
	build/ext-data-control-v1-client-protocol.h build/viewporter-client-protocol.h
PROTO_C = build/xdg-shell-protocol.c build/xdg-decoration-unstable-v1-protocol.c \
	build/ext-data-control-v1-protocol.c build/viewporter-protocol.c

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g
MFM_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
MFM_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: mfm

mfm: mfm.c seats.c seats.h theme.c theme.h version.h $(PROTO_H) $(PROTO_C)
	$(CC) $(MFM_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mfm.c seats.c theme.c $(PROTO_C) \
		$(MFM_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/xdg-shell-client-protocol.h: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL) $@

build/xdg-shell-protocol.c: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@

build/xdg-decoration-unstable-v1-client-protocol.h: $(XDG_DECORATION) | build
	$(WAYLAND_SCANNER) client-header $(XDG_DECORATION) $@

build/xdg-decoration-unstable-v1-protocol.c: $(XDG_DECORATION) | build
	$(WAYLAND_SCANNER) private-code $(XDG_DECORATION) $@

build/ext-data-control-v1-client-protocol.h: $(DATA_CONTROL) | build
	$(WAYLAND_SCANNER) client-header $(DATA_CONTROL) $@

build/ext-data-control-v1-protocol.c: $(DATA_CONTROL) | build
	$(WAYLAND_SCANNER) private-code $(DATA_CONTROL) $@

build/viewporter-client-protocol.h: $(VIEWPORTER) | build
	$(WAYLAND_SCANNER) client-header $(VIEWPORTER) $@

build/viewporter-protocol.c: $(VIEWPORTER) | build
	$(WAYLAND_SCANNER) private-code $(VIEWPORTER) $@

# For the tests: what is in the clipboard.
build/clipget: tests/clipget.c build/ext-data-control-v1-client-protocol.h \
		build/ext-data-control-v1-protocol.c
	$(CC) $(MFM_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/clipget.c \
		build/ext-data-control-v1-protocol.c $(MFM_LIBS) $(LDLIBS)

# The modules without Wayland, each under ASan and UBSan.
UNIT_CFLAGS = -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	-fno-sanitize-recover=all
UNITS = build/test_seats build/test_theme

build/test_seats: tests/test_seats.c seats.c seats.h | build
	$(CC) $(UNIT_CFLAGS) -I. -o $@ tests/test_seats.c seats.c

build/test_theme: tests/test_theme.c theme.c theme.h | build
	$(CC) $(UNIT_CFLAGS) -I. -o $@ tests/test_theme.c theme.c -lm

check: $(UNITS)
	@for t in $(UNITS); do echo "$$t"; ./$$t || exit 1; done

install: mfm
	install -Dm755 mfm $(DESTDIR)$(PREFIX)/bin/mfm
	install -d $(DESTDIR)$(PREFIX)/share/applications
	sed 's|^Exec=mfm |Exec=$(PREFIX)/bin/mfm |' data/mfm.desktop \
		>$(DESTDIR)$(PREFIX)/share/applications/mfm.desktop
	chmod 644 $(DESTDIR)$(PREFIX)/share/applications/mfm.desktop
	install -Dm644 mfm.1 $(DESTDIR)$(PREFIX)/share/man/man1/mfm.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mfm \
		$(DESTDIR)$(PREFIX)/share/applications/mfm.desktop \
		$(DESTDIR)$(PREFIX)/share/man/man1/mfm.1

# Needs a built mwc next to this directory, or in MWC_DIR, grim and pgrep. files.sh
# also needs unprivileged user namespaces, and fails without them.
test: mfm build/clipget
	tests/ui.sh
	tests/files.sh

# The same in a podman (or docker) container, image mfm-test; see tests/docker/run.sh.
test-docker:
	tests/docker/run.sh

clean:
	rm -rf mfm build

.PHONY: all install uninstall check test test-docker clean
