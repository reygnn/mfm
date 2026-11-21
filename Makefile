# mfm - minimal file manager for Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
MTK_DIR = mtk

all: mfm

include $(MTK_DIR)/mtk.mk

# The clipboard of the session, which a child serves on a connection of its own.
DATA_CONTROL = $(MTK_PROTOCOLS)/staging/ext-data-control/ext-data-control-v1.xml
PROTO_H = build/ext-data-control-v1-client-protocol.h
PROTO_C = build/ext-data-control-v1-protocol.c

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g
MFM_CFLAGS = $(MTK_CFLAGS) -Ibuild

mfm: mfm.c theme.c theme.h version.h $(PROTO_H) $(PROTO_C) $(MTK_DEPS)
	$(CC) $(MFM_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ mfm.c theme.c \
		$(PROTO_C) $(MTK_SRC) $(MTK_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/ext-data-control-v1-client-protocol.h: $(DATA_CONTROL) | build
	$(MTK_SCANNER) client-header $(DATA_CONTROL) $@

build/ext-data-control-v1-protocol.c: $(DATA_CONTROL) | build
	$(MTK_SCANNER) private-code $(DATA_CONTROL) $@

# For the tests: what is in the clipboard.
build/clipget: tests/clipget.c $(PROTO_H) $(PROTO_C)
	$(CC) $(MFM_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/clipget.c \
		$(PROTO_C) $(MTK_LIBS) $(LDLIBS)

# The module without Wayland under ASan and UBSan, with mtk's that it uses.
UNIT_CFLAGS = -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	-fno-sanitize-recover=all
UNITS = build/test_theme

build/test_theme: tests/test_theme.c theme.c theme.h $(MTK_HEADLESS) $(MTK_DIR)/mtk.h | build
	$(CC) $(UNIT_CFLAGS) -I. -I$(MTK_DIR) \
		$(shell $(PKG_CONFIG) --cflags $(MTK_HEADLESS_PKGS)) -o $@ \
		tests/test_theme.c theme.c $(MTK_HEADLESS) \
		$(shell $(PKG_CONFIG) --libs $(MTK_HEADLESS_PKGS)) -lm

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
