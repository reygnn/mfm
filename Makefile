# mfm - minimal file manager for X11 (mwm look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = x11 xft

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

all: mfm

mfm: mfm.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ mfm.c $(LDLIBS)

install: mfm
	install -Dm755 mfm $(DESTDIR)$(PREFIX)/bin/mfm

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mfm

clean:
	rm -f mfm

.PHONY: all install uninstall clean
