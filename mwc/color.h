/*
 * color.h: the colours mwc and its clients share, on 0xrrggbb: the built-in defaults
 * and Motif 2.3.8's title text and shadows. No dependencies beyond the C library, so
 * mtk takes color.c and color.h unchanged; mwc is where they are changed.
 */
#ifndef MWC_COLOR_H
#define MWC_COLOR_H

#include <stdbool.h>
#include <stdint.h>

/* IRIX 6.5's Base scheme (IndigoMagic); the desktop is mwc's own. */
#define COLOR_ACTIVE 0xc6c1aau
#define COLOR_INACTIVE 0xaaaaaau
#define COLOR_ROOT 0x5b7792u

struct color_shades {
	uint32_t top, bottom;
};

/* Motif's title text on bg: black above 70% of its brightness, white otherwise. */
uint32_t color_text(uint32_t bg);

/* The text for unfocused frames, menus and icons with no foreground set: black on the
 * built-in COLOR_INACTIVE, as the Base palette has it, else color_text(bg). builtin
 * says that bg is the built-in one because nothing good was set, not that it equals it. */
uint32_t color_inactive_text(uint32_t bg, bool builtin);

/* The top and bottom shadows of bg as Motif computes them, cut to 8 bits as a TrueColor
 * screen shows them. */
struct color_shades color_shades(uint32_t bg);

#endif
