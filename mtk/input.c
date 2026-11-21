/* input.c - keys, key repeat, seats and cursor sizes, without Wayland. */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "mtk.h"

#ifndef MAX            /* glib has it too */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

int mtk_cursor_size(const char *s)
{
	char *end;
	long v = s ? strtol(s, &end, 10) : 0;
	if (!s || end == s || *end || v <= 0)
		return 24;
	return v > 256 ? 256 : (int)v;
}

void mtk_cursor_fit(int w, int h, int s, int has_vp, int *buf_scale, int *vw,
	int *vh)
{
	*buf_scale = 1;
	*vw = *vh = -1;
	if (w % s == 0 && h % s == 0) {
		*buf_scale = s;
	} else if (has_vp) {
		*vw = MAX((w + s / 2) / s, 1);
		*vh = MAX((h + s / 2) / s, 1);
	} else {
		for (int d = s - 1; d > 1; d--)
			if (w % d == 0 && h % d == 0) {
				*buf_scale = d;
				break;
			}
	}
}

int mtk_buffer_scale(int factor)
{
	if (factor < 1)
		return 0;
	return factor > 16 ? 16 : factor;       /* no output is denser */
}

static int size_in(int v, int min)
{
	v = v < min ? min : v;
	return v > MTK_MAX_SIZE ? MTK_MAX_SIZE : v;
}

void mtk_configure_size(int cw, int ch, unsigned states, int min_w, int min_h,
	int *fw, int *fh, int *w, int *h)
{
	*w = size_in(cw > 0 ? cw : *fw, min_w);
	*h = size_in(ch > 0 ? ch : *fh, min_h);
	if (!(states & (MTK_STATE_MAXIMIZED | MTK_STATE_FULLSCREEN | MTK_STATE_TILED))) {
		*fw = *w;
		*fh = *h;
	}
}

int mtk_draw_scale(int w, int h, int s)
{
	while (s > 1 && (w > MTK_CAIRO_MAX / s || h > MTK_CAIRO_MAX / s ||
	    (int64_t)w * s * 4 * h * s > INT32_MAX))
		s--;
	return s;
}

int mtk_seats_add(struct mtk_seats *s, unsigned name, unsigned ver)
{
	if (s->n == MTK_MAX_SEATS)
		return 0;
	s->name[s->n] = name;
	s->ver[s->n] = ver;
	s->n++;
	return 1;
}

void mtk_seats_remove(struct mtk_seats *s, unsigned name)
{
	for (int i = 0; i < s->n; i++)
		if (s->name[i] == name) {
			for (s->n--; i < s->n; i++) {
				s->name[i] = s->name[i + 1];
				s->ver[i] = s->ver[i + 1];
			}
			return;
		}
}

int mtk_seats_next(const struct mtk_seats *s, unsigned *name, unsigned *ver)
{
	if (!s->n)
		return 0;
	*name = s->name[0];
	*ver = s->ver[0];
	return 1;
}

int mtk_key_quits(xkb_keysym_t ks, unsigned mods)
{
	if (mods & (MTK_MOD_ALT | MTK_MOD_LOGO))
		return 0;       /* Alt+Q must not quit */
	if (mods & MTK_MOD_CTRL)
		return ks == XKB_KEY_q || ks == XKB_KEY_Q || ks == XKB_KEY_w ||
			ks == XKB_KEY_W;
	return ks == XKB_KEY_q || ks == XKB_KEY_Q || ks == XKB_KEY_Escape;
}

unsigned mtk_mods(struct xkb_state *st)
{
	static const struct { const char *name; unsigned bit; } m[] = {
		{ XKB_MOD_NAME_SHIFT, MTK_MOD_SHIFT }, { XKB_MOD_NAME_CTRL, MTK_MOD_CTRL },
		{ XKB_MOD_NAME_ALT, MTK_MOD_ALT }, { XKB_MOD_NAME_LOGO, MTK_MOD_LOGO },
	};
	unsigned out = 0;
	for (size_t i = 0; st && i < sizeof m / sizeof m[0]; i++)
		if (xkb_state_mod_name_is_active(st, m[i].name,
		    XKB_STATE_MODS_EFFECTIVE) > 0)
			out |= m[i].bit;
	return out;
}

/* The locale compose sequences follow, in the order of xkbcommon's docs. */
struct xkb_compose_state *mtk_compose_new(struct xkb_context *ctx)
{
	const char *loc = getenv("LC_ALL");
	if (!loc || !*loc)
		loc = getenv("LC_CTYPE");
	if (!loc || !*loc)
		loc = getenv("LANG");
	if (!loc || !*loc)
		loc = "C";
	struct xkb_compose_table *table = xkb_compose_table_new_from_locale(ctx, loc,
		XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (!table)
		return NULL;
	struct xkb_compose_state *st = xkb_compose_state_new(table,
		XKB_COMPOSE_STATE_NO_FLAGS);
	xkb_compose_table_unref(table);
	return st;
}

static int is_char(xkb_keysym_t ks)
{
	uint32_t c = xkb_keysym_to_utf32(ks);
	return c >= 0x20 && c != 0x7f;
}

int mtk_key_translate(struct xkb_state *st, struct xkb_compose_state *compose,
	uint32_t kc, xkb_keysym_t *sym, char utf8[64])
{
	int n = -1;
	xkb_keysym_t ks = xkb_state_key_get_one_sym(st, kc);
	utf8[0] = 0;
	if (compose && xkb_compose_state_feed(compose, ks) ==
	    XKB_COMPOSE_FEED_ACCEPTED) {
		switch (xkb_compose_state_get_status(compose)) {
		case XKB_COMPOSE_COMPOSING:
			return 0;
		case XKB_COMPOSE_CANCELLED:
			xkb_compose_state_reset(compose);
			/* A character that ends a sequence it doesn't fit is dropped, as
			 * elsewhere; Esc, Return or an arrow still does what it does. */
			if (is_char(ks))
				return 0;
			break;
		case XKB_COMPOSE_COMPOSED:
			n = xkb_compose_state_get_utf8(compose, utf8, 64);
			ks = xkb_compose_state_get_one_sym(compose);
			xkb_compose_state_reset(compose);
			break;
		case XKB_COMPOSE_NOTHING:
			break;
		}
	}
	if (n < 0)
		n = xkb_state_key_get_utf8(st, kc, utf8, 64);
	if (n < 0 || n >= 64)
		utf8[0] = 0;    /* none, or cut: not a part of a character */
	*sym = ks;
	return 1;
}

xkb_keysym_t mtk_latin_sym(struct xkb_keymap *map, uint32_t kc, xkb_keysym_t ks)
{
	if ((ks < 0x80 && isalnum((int)ks)) || !map)
		return ks;
	xkb_layout_index_t n = xkb_keymap_num_layouts_for_key(map, kc);
	for (xkb_layout_index_t l = 0; l < n; l++)
		for (xkb_level_index_t lv = 0; lv < 2; lv++) {
			const xkb_keysym_t *syms;
			if (xkb_keymap_key_get_syms_by_level(map, kc, l, lv, &syms) == 1 &&
			    syms[0] < 0x80 && (isdigit((int)syms[0]) ||
			    (lv == 0 && isalpha((int)syms[0]))))
				return syms[0];
		}
	if (ks < 0x100)
		return ks;
	for (xkb_layout_index_t l = 0; l < n; l++) {
		const xkb_keysym_t *syms;
		if (xkb_keymap_key_get_syms_by_level(map, kc, l, 0, &syms) == 1 &&
		    syms[0] > 0x20 && syms[0] < 0x7f)
			return syms[0];
	}
	return ks;
}

void mtk_repeat_init(struct mtk_repeat *r)
{
	*r = (struct mtk_repeat){ .rate = 25, .delay = 600 };
}

void mtk_repeat_info(struct mtk_repeat *r, int rate, int delay)
{
	r->rate = rate;
	r->delay = delay < 0 ? 0 : delay;
	/* 0 turns it off, also for a key that is repeating now. */
	if (rate <= 0)
		r->kc = 0;
}

void mtk_repeat_press(struct mtk_repeat *r, uint32_t kc, int keymap_repeats,
	int may_repeat, uint64_t now)
{
	if (!keymap_repeats)
		return;         /* a modifier */
	r->kc = 0;
	if (may_repeat && r->rate > 0) {
		r->kc = kc;
		r->next = now + (uint64_t)r->delay;
	}
}

void mtk_repeat_release(struct mtk_repeat *r, uint32_t kc)
{
	if (kc == r->kc)
		r->kc = 0;
}

void mtk_repeat_stop(struct mtk_repeat *r)
{
	r->kc = 0;
}

uint32_t mtk_repeat_due(struct mtk_repeat *r, uint64_t now)
{
	if (!r->kc || now < r->next)
		return 0;
	r->next = now + (uint64_t)MAX(1000 / MAX(r->rate, 1), 1);
	return r->kc;
}

int mtk_repeat_timeout(const struct mtk_repeat *r, uint64_t now)
{
	if (!r->kc)
		return -1;
	if (r->next <= now)
		return 0;
	uint64_t d = r->next - now;
	return d > INT_MAX ? INT_MAX : (int)d;
}

static int clamp_steps(long v)
{
	return v > MTK_SCROLL_MAX ? MTK_SCROLL_MAX : v < -MTK_SCROLL_MAX ? -MTK_SCROLL_MAX :
		(int)v;
}

void mtk_scroll_value(struct mtk_scroll *s, double v)
{
	s->acc += v;
}

void mtk_scroll_discrete(struct mtk_scroll *s, int steps)
{
	s->steps = clamp_steps((long)s->steps + clamp_steps(steps));
}

void mtk_scroll_stop(struct mtk_scroll *s)
{
	s->acc = 0;
}

int mtk_scroll_frame(struct mtk_scroll *s)
{
	int steps = s->steps;
	s->steps = 0;
	if (steps) {
		s->acc = 0;
		return steps;
	}
	double q = s->acc / MTK_SCROLL_STEP;
	/* A flood, or a value no int holds, leaves nothing to carry on. */
	if (!isfinite(q) || fabs(q) > MTK_SCROLL_MAX) {
		s->acc = 0;
		return !isfinite(q) ? 0 : q > 0 ? MTK_SCROLL_MAX : -MTK_SCROLL_MAX;
	}
	steps = (int)q;
	s->acc -= steps * MTK_SCROLL_STEP;
	return steps;
}
