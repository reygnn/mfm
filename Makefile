# mfm - minimal file manager for Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wayland-client wayland-cursor xkbcommon cairo pangocairo

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
XDG_DECORATION = $(WAYLAND_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml

PROTO_H = build/xdg-shell-client-protocol.h \
	build/xdg-decoration-unstable-v1-client-protocol.h
PROTO_C = build/xdg-shell-protocol.c build/xdg-decoration-unstable-v1-protocol.c

# CFLAGS and LDLIBS stay the user's, also on the command line: `make CFLAGS=-O0`
# must not lose the include paths.
CFLAGS ?= -O2 -g
MFM_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
MFM_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS))

all: mfm

mfm: mfm.c $(PROTO_H) $(PROTO_C)
	$(CC) $(MFM_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mfm.c $(PROTO_C) $(MFM_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/xdg-shell-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL) $@

build/xdg-shell-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@

build/xdg-decoration-unstable-v1-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(XDG_DECORATION) $@

build/xdg-decoration-unstable-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_DECORATION) $@

install: mfm
	install -Dm755 mfm $(DESTDIR)$(PREFIX)/bin/mfm
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	sed 's|^Exec=mfm |Exec=$(PREFIX)/bin/mfm |' data/mfm.desktop \
		>$(DESTDIR)$(PREFIX)/share/applications/mfm.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mfm \
		$(DESTDIR)$(PREFIX)/share/applications/mfm.desktop

# Needs a built mwc next to this directory, or in MWC_DIR, and grim. files.sh also
# needs unprivileged user namespaces, and is skipped without them.
test: mfm
	tests/ui.sh
	tests/files.sh

clean:
	rm -rf mfm build

.PHONY: all install uninstall test clean
