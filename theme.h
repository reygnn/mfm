/* theme.h - the faces and the text on them, from mwc's colours: make check */
#ifndef THEME_H
#define THEME_H

#include <stdint.h>

struct theme {
	/* faces */
	uint32_t bg, top, bot, list, trough, sel, mark, folder, paper;
	/* text, by the face it is on */
	uint32_t fg, dim, err;          /* bg: status line, menu */
	uint32_t list_fg, list_dim;     /* list, path field */
	uint32_t mark_fg, mark_dim;     /* a marked row */
	uint32_t sel_fg;                /* sel */
	uint32_t paper_fg;              /* input fields */
};

/* fg and sel_fg are the user's colours, or NULL for black or white. */
void theme_make(struct theme *t, uint32_t bg, uint32_t act, const uint32_t *fg,
	const uint32_t *sel_fg);

uint32_t mix(uint32_t rgb, uint32_t to, float f);
float contrast_ratio(uint32_t a, uint32_t b);
/* Black or white, whichever reads better on bg. */
uint32_t contrast(uint32_t bg);

#endif
