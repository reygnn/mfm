/* theme.c - the faces and the text on them, by mwc's rules through mtk */
#include "theme.h"

void theme_make(struct theme *t, uint32_t bg, uint32_t act, const uint32_t *fg,
	const uint32_t *sel_fg)
{
	struct mtk_shade s = mtk_shade(bg, 0);
	t->bg = bg;
	t->top = s.top;
	t->bot = s.bot;
	t->list = mtk_mix(bg, 0xffffff, 0.3f);
	t->trough = mtk_mix(bg, 0x000000, 0.12f);
	t->sel = act;
	t->mark = mtk_mix(t->list, act, 0.4f);
	t->folder = 0xc9a85c;
	t->paper = 0xf6f6f6;

	/* On mwc's faces, the background of the status line and the menu and the
	 * armed item's colour, the user's colours as they are, as mwc takes them for
	 * its titles, else black or white as Motif picks it. The list, the marks
	 * and the input fields are mfm's own faces: the user's colour moved as far
	 * as it needs to read there, as pulseControl does, else black or white,
	 * whichever reads better. */
	t->fg = fg ? *fg : mtk_text_color(bg);
	t->sel_fg = sel_fg ? *sel_fg : mtk_text_color(act);
	t->list_fg = fg ? mtk_readable(t->list, *fg, mtk_best_contrast(t->list)) :
		mtk_best_contrast(t->list);
	t->mark_fg = fg ? mtk_readable(t->mark, *fg, mtk_best_contrast(t->mark)) :
		mtk_best_contrast(t->mark);
	t->paper_fg = mtk_readable(t->paper, t->fg, mtk_best_contrast(t->paper));
	t->dim = mtk_dimmed(bg, t->fg);
	t->list_dim = mtk_dimmed(t->list, t->list_fg);
	t->mark_dim = mtk_dimmed(t->mark, t->mark_fg);
	/* by the background, not fg: readable on dark themes */
	t->err = mtk_readable(bg, mtk_best_contrast(bg) == 0xffffff ? 0xffa0a0 : 0x8a0303,
		mtk_best_contrast(bg));
}
