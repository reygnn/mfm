# mtk.mk - mtk for a client's Makefile, where mtk is vendored in MTK_DIR:
#
#   all: app
#   MTK_DIR = mtk
#   include $(MTK_DIR)/mtk.mk
#   app: app.c $(MTK_DEPS)
#   	$(CC) $(MTK_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ app.c \
#   		$(MTK_SRC) $(MTK_LIBS) $(LDLIBS)
#
# The protocol code goes to MTK_BUILD. PKG_CONFIG and CC are the client's. The
# client's default goal stays its own, wherever it includes this.

MTK_GOAL := $(.DEFAULT_GOAL)
MTK_DIR ?= mtk
MTK_BUILD ?= build/mtk
PKG_CONFIG ?= pkg-config

MTK_PKGS = wayland-client wayland-cursor xkbcommon cairo pangocairo
MTK_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
MTK_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
MTK_XML = $(MTK_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml \
	$(MTK_PROTOCOLS)/stable/viewporter/viewporter.xml \
	$(MTK_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
MTK_PROTO_NAMES = xdg-shell viewporter xdg-decoration-unstable-v1
MTK_PROTO_H = $(MTK_PROTO_NAMES:%=$(MTK_BUILD)/%-client-protocol.h)
MTK_PROTO_C = $(MTK_PROTO_NAMES:%=$(MTK_BUILD)/%-protocol.c)

# The modules without Wayland, which a client's tests may link alone.
MTK_HEADLESS = $(MTK_DIR)/color.c $(MTK_DIR)/rc.c $(MTK_DIR)/draw.c \
	$(MTK_DIR)/input.c $(MTK_DIR)/field.c
MTK_SRC = $(MTK_HEADLESS) $(MTK_DIR)/wl.c $(MTK_PROTO_C)
MTK_DEPS = $(MTK_SRC) $(MTK_DIR)/mtk.h $(MTK_PROTO_H)
MTK_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -I$(MTK_DIR) -I$(MTK_BUILD) \
	$(shell $(PKG_CONFIG) --cflags $(MTK_PKGS))
MTK_LIBS = $(shell $(PKG_CONFIG) --libs $(MTK_PKGS)) -lm
MTK_HEADLESS_PKGS = xkbcommon cairo pangocairo

$(MTK_BUILD):
	mkdir -p $@

$(MTK_BUILD)/xdg-shell-%: $(MTK_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml | $(MTK_BUILD)
	$(MTK_SCANNER) $(if $(filter %.h,$@),client-header,private-code) $< $@

$(MTK_BUILD)/viewporter-%: $(MTK_PROTOCOLS)/stable/viewporter/viewporter.xml | $(MTK_BUILD)
	$(MTK_SCANNER) $(if $(filter %.h,$@),client-header,private-code) $< $@

$(MTK_BUILD)/xdg-decoration-unstable-v1-%: \
		$(MTK_PROTOCOLS)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml \
		| $(MTK_BUILD)
	$(MTK_SCANNER) $(if $(filter %.h,$@),client-header,private-code) $< $@

.DEFAULT_GOAL := $(MTK_GOAL)
