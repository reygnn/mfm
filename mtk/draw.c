/* draw.c - mwm's bevels, borders and text on Cairo, without Wayland. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtk.h"

void mtk_set_rgb(cairo_t *cr, uint32_t c)
{
	cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0,
		(c & 0xff) / 255.0);
}

void mtk_fill(cairo_t *cr, uint32_t c, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	mtk_set_rgb(cr, c);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

/* Row i of the top runs to the right edge, the right side starts below it: the
 * diagonal pixel at the top right is tl's, and so at the bottom left. */
void mtk_bevel(cairo_t *cr, int x, int y, int w, int h, int t, uint32_t tl,
	uint32_t br)
{
	for (int i = 0; i < t && 2 * i < w && 2 * i < h; i++) {
		mtk_fill(cr, tl, x + i, y + i, w - 2 * i, 1);
		mtk_fill(cr, tl, x + i, y + i, 1, h - 2 * i);
		mtk_fill(cr, br, x + i + 1, y + h - 1 - i, w - 2 * i - 1, 1);
		mtk_fill(cr, br, x + w - 1 - i, y + i + 1, 1, h - 2 * i - 1);
	}
}

void mtk_shadow(cairo_t *cr, int x, int y, int w, int h, int t,
	const struct mtk_shade *s, int in)
{
	mtk_bevel(cr, x, y, w, h, t, in ? s->bot : s->top, in ? s->top : s->bot);
}

int mtk_damage_rows(const void *now, const void *was, size_t stride, int h, int gap,
	int max, int rows[][2])
{
	const char *p = now, *q = was;
	int y0 = 0, y1 = h;
	/* The ends first, so only what lies between the first and the last change is
	 * read for gaps. */
	while (y0 < y1 && !memcmp(p + y0 * stride, q + y0 * stride, stride))
		y0++;
	while (y1 > y0 && !memcmp(p + (y1 - 1) * stride, q + (y1 - 1) * stride, stride))
		y1--;
	if (y1 == y0 || max < 1)
		return 0;
	int n = 0, start = y0, last = y0;
	for (int y = y0 + 1; y < y1 && n < max - 1; y++) {
		if (!memcmp(p + y * stride, q + y * stride, stride))
			continue;
		if (y - last > gap) {
			rows[n][0] = start;
			rows[n][1] = last + 1 - start;
			n++;
			start = y;
		}
		last = y;
	}
	rows[n][0] = start;
	rows[n][1] = y1 - start;
	return n + 1;
}

void mtk_groove_h(cairo_t *cr, int x, int y, int w, const struct mtk_shade *s)
{
	mtk_fill(cr, s->bot, x, y, w, 1);
	mtk_fill(cr, s->top, x, y + 1, w, 1);
}

void mtk_groove_v(cairo_t *cr, int x, int y, int h, const struct mtk_shade *s)
{
	mtk_fill(cr, s->bot, x, y, 1, h);
	mtk_fill(cr, s->top, x + 1, y, 1, h);
}

void mtk_border(cairo_t *cr, int w, int h, const struct mtk_shade *s)
{
	mtk_fill(cr, s->bg, 0, 0, w, MTK_BORDER);
	mtk_fill(cr, s->bg, 0, h - MTK_BORDER, w, MTK_BORDER);
	mtk_fill(cr, s->bg, 0, 0, MTK_BORDER, h);
	mtk_fill(cr, s->bg, w - MTK_BORDER, 0, MTK_BORDER, h);
	mtk_shadow(cr, 0, 0, w, h, MTK_BEVEL, s, 0);
}

/* The family Pango would take for d. */
static int font_is(PangoFontDescription *d, const char *family, char *got, size_t n)
{
	PangoContext *pctx = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	PangoFont *pf = pango_context_load_font(pctx, d);
	PangoFontDescription *fd = pf ? pango_font_describe(pf) : NULL;
	const char *fam = fd ? pango_font_description_get_family(fd) : NULL;
	snprintf(got, n, "%s", fam ? fam : "none");
	int ok = fam && !g_ascii_strcasecmp(fam, family);
	if (fd)
		pango_font_description_free(fd);
	if (pf)
		g_object_unref(pf);
	g_object_unref(pctx);
	return ok;
}

#define DOCK_FONT_FAMILY "DejaVu Sans"
#define DOCK_FONT_SIZE 10       /* points */

PangoFontDescription *mtk_dock_font(const char *name)
{
	PangoFontDescription *d = pango_font_description_new();
	pango_font_description_set_family(d, DOCK_FONT_FAMILY);
	pango_font_description_set_style(d, PANGO_STYLE_NORMAL);
	pango_font_description_set_variant(d, PANGO_VARIANT_NORMAL);
	pango_font_description_set_weight(d, PANGO_WEIGHT_NORMAL);
	pango_font_description_set_stretch(d, PANGO_STRETCH_NORMAL);
	pango_font_description_set_size(d, DOCK_FONT_SIZE * PANGO_SCALE);
	/* Without it fontconfig quietly takes another font, and the dock may not line
	 * up; say so. */
	char got[128];
	if (!font_is(d, DOCK_FONT_FAMILY, got, sizeof got))
		fprintf(stderr, "%s: font %s not found, using %s\n", name,
			DOCK_FONT_FAMILY, got);
	return d;
}

PangoFontDescription *mtk_font(const char *name, const char *pango_name)
{
	PangoFontDescription *d = pango_font_description_from_string(pango_name);
	const char *fam = pango_font_description_get_family(d);
	char got[128];
	/* A family list ("Sans, Noto") is left to fontconfig. */
	if (!fam || strchr(fam, ',') || font_is(d, fam, got, sizeof got) ||
	    /* The generic names are aliases, never the family itself. */
	    !g_ascii_strcasecmp(fam, "sans") || !g_ascii_strcasecmp(fam, "serif") ||
	    !g_ascii_strcasecmp(fam, "monospace") || !g_ascii_strcasecmp(fam, "sans-serif"))
		return d;
	fprintf(stderr, "%s: font \"%s\" not found, Pango would take %s\n", name,
		pango_name, got);
	pango_font_description_free(d);
	return NULL;
}

PangoLayout *mtk_layout_new(const PangoFontDescription *font)
{
	PangoContext *pctx = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	PangoLayout *l = pango_layout_new(pctx);
	g_object_unref(pctx);   /* the layout keeps its own reference */
	pango_layout_set_single_paragraph_mode(l, TRUE);
	if (font)
		pango_layout_set_font_description(l, font);
	return l;
}

int mtk_text_width(PangoLayout *l, const char *s)
{
	int w;
	pango_layout_set_text(l, s, -1);
	pango_layout_get_pixel_size(l, &w, NULL);
	return w;
}

void mtk_text(cairo_t *cr, PangoLayout *l, const char *s, uint32_t c, int x, int y)
{
	pango_layout_set_text(l, s, -1);
	mtk_set_rgb(cr, c);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, l);
}

/* ---- a field: mfm's draw_input() ---------------------------------------- */

struct fit {
	PangoLayout *l;
	const char *s;
	int n;
	unsigned char *stop;            /* where s may be cut: grapheme boundaries */
};

static int fit_shape(struct fit *f, const char *s, int n)
{
	int w;
	pango_layout_set_text(f->l, s, n > 0 ? n : 0);
	pango_layout_get_pixel_size(f->l, &w, NULL);
	return n > 0 ? w : 0;
}

static int stop_at(struct fit *f, const char *s, int m)
{
	long at = s - f->s + m;
	return at >= 0 && at <= f->n ? f->stop[at] : (s[m] & 0xc0) != 0x80;
}

/* A place to cut s between lo and hi, not lo or hi themselves, near the middle;
 * -1 if there is none. */
static int mid_char(struct fit *f, const char *s, int lo, int hi)
{
	int m = lo + (hi - lo) / 2;
	while (m > lo && !stop_at(f, s, m))
		m--;
	if (m == lo)
		for (m = lo + 1; m < hi && !stop_at(f, s, m); m++)
			;
	return m < hi ? m : -1;
}

/* The end of the longest prefix of s[0..n) at most maxw wide, by halves: shaping
 * is too slow to go through it by the character. */
static int fit_prefix(struct fit *f, const char *s, int n, int maxw)
{
	if (fit_shape(f, s, n) <= maxw)
		return n;
	int lo = 0, hi = n, m;
	while ((m = mid_char(f, s, lo, hi)) >= 0) {
		if (fit_shape(f, s, m) <= maxw)
			lo = m;
		else
			hi = m;
	}
	return lo;
}

/* The start of the longest suffix of s[0..n) at most maxw wide. */
static int fit_suffix(struct fit *f, const char *s, int n, int maxw)
{
	if (fit_shape(f, s, n) <= maxw)
		return 0;
	int lo = 0, hi = n, m;
	while ((m = mid_char(f, s, lo, hi)) >= 0) {
		if (fit_shape(f, s + m, n - m) <= maxw)
			hi = m;
		else
			lo = m;
	}
	return hi;
}

/* s[0..n) in c at x, its line centred in y..y + h by the font's ascent and
 * descent. */
static void fit_text(cairo_t *cr, struct fit *f, int asc, int desc, uint32_t c,
	int x, int y, int h, const char *s, int n)
{
	if (n <= 0)
		return;
	pango_layout_set_text(f->l, s, n);
	int base = y + (h - (asc + desc)) / 2 + asc;
	mtk_set_rgb(cr, c);
	cairo_move_to(cr, x, base - pango_layout_get_baseline(f->l) / PANGO_SCALE);
	pango_cairo_show_layout(cr, f->l);
}

void mtk_field_draw(cairo_t *cr, PangoLayout *l, const char *d, size_t cur,
	int selected, int x, int y, int w, int h, uint32_t fg, uint32_t sel_bg,
	uint32_t sel_fg)
{
	int n = strlen(d), dc = cur < (size_t)n ? (int)cur : n;
	struct fit f = { l, d, n, calloc(n + 1, 1) };
	if (!f.stop)
		return;
	PangoContext *pctx = pango_layout_get_context(l);
	PangoFontMetrics *fm = pango_context_get_metrics(pctx,
		pango_layout_get_font_description(l), NULL);
	int asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(fm));
	int desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(fm));
	pango_font_metrics_unref(fm);
	int ew = fit_shape(&f, "...", 3), maxw = w - 2, start = 0, end = n, x0 = x;
	if (maxw <= 2 * ew) {
		free(f.stop);
		return;
	}
	glong nch = g_utf8_strlen(d, n);
	PangoLogAttr *a = g_new(PangoLogAttr, nch + 1);
	pango_get_log_attrs(d, n, -1, pango_language_get_default(), a, nch + 1);
	const char *p = d;
	for (glong i = 0; i <= nch; i++, p = g_utf8_next_char(p))
		f.stop[p - d] = a[i].is_cursor_position || i == nch;
	g_free(a);
	/* The cursor goes to the start of the grapheme it is in. */
	while (dc > 0 && !f.stop[dc])
		dc--;
	if (fit_prefix(&f, d, n, maxw) < n) {
		if (fit_prefix(&f, d, dc, maxw - ew) < dc) {
			start = fit_suffix(&f, d, dc, maxw - 2 * ew);
			fit_text(cr, &f, asc, desc, fg, x, y, h, "...", 3);
			x += ew;
		}
		end = start + fit_prefix(&f, d + start, n - start, maxw - (x - x0) - ew);
	}
	/* One layout for all that shows: cut at the cursor, right to left text would
	 * come apart. Shaping need not grow with the text, so the cut can fall short
	 * of the cursor: it goes at the end then. */
	end = end > dc ? end : dc;
	int seg = end - start, c = dc - start;
	PangoRectangle at = { 0 };
	pango_layout_set_text(l, d + start, seg);
	pango_layout_get_cursor_pos(l, c, &at, NULL);
	if (selected && c > 0) {
		int *ranges, nr;
		pango_layout_line_get_x_ranges(pango_layout_get_line_readonly(l, 0), 0, c,
			&ranges, &nr);
		for (int i = 0; i < nr; i++)
			mtk_fill(cr, sel_bg, x + PANGO_PIXELS(ranges[2 * i]), y + 2,
				PANGO_PIXELS(ranges[2 * i + 1] - ranges[2 * i]), h - 4);
		g_free(ranges);
		PangoAttrList *attrs = pango_attr_list_new();
		PangoAttribute *af = pango_attr_foreground_new(
			((sel_fg >> 16) & 0xff) * 257, ((sel_fg >> 8) & 0xff) * 257,
			(sel_fg & 0xff) * 257);
		af->start_index = 0;
		af->end_index = c;
		pango_attr_list_insert(attrs, af);
		pango_layout_set_attributes(l, attrs);
		pango_attr_list_unref(attrs);
	}
	fit_text(cr, &f, asc, desc, fg, x, y, h, d + start, seg);
	pango_layout_set_attributes(l, NULL);
	if (end < n)
		fit_text(cr, &f, asc, desc, fg, x + fit_shape(&f, d + start, seg), y, h,
			"...", 3);
	mtk_fill(cr, fg, x + PANGO_PIXELS(at.x), y + 3, 1, h - 6);
	free(f.stop);
}
