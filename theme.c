/* theme.c - the faces and the text on them (same rules as mwm's draw.c) */
#include <math.h>

#include "theme.h"

uint32_t mix(uint32_t rgb, uint32_t to, float f)
{
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float c = (rgb >> sh) & 0xff, t = (to >> sh) & 0xff;
		out |= (uint32_t)(c + (t - c) * f + 0.5f) << sh;
	}
	return out;
}

/* An sRGB channel as linear light. */
static float linear(unsigned c)
{
	float v = c / 255.0f;
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static float luminance(uint32_t c)
{
	return 0.2126f * linear((c >> 16) & 0xff) +
		0.7152f * linear((c >> 8) & 0xff) + 0.0722f * linear(c & 0xff);
}

float contrast_ratio(uint32_t a, uint32_t b)
{
	float la = luminance(a) + 0.05f, lb = luminance(b) + 0.05f;
	return la > lb ? la / lb : lb / la;
}

/* The two are equal at a relative luminance of sqrt(1.05 * 0.05) - 0.05, a grey
 * of about 0x76: a mid-tone such as mwc's activeBackground gets black. Either
 * has 4.5:1 on any background. */
uint32_t contrast(uint32_t bg)
{
	float l = luminance(bg);
	return (l + 0.05f) * (l + 0.05f) > 1.05f * 0.05f ? 0x000000 : 0xffffff;
}

/* Dimmed towards fg, but no further than WCAG AA's 4.5:1 for small text
 * allows; fg itself if even that falls short. The same steps as the dock
 * tools, so they match. */
static uint32_t dimmed(uint32_t bg, uint32_t fg)
{
	for (int k = 12; k < 20; k++) {
		uint32_t c = mix(bg, fg, k / 20.0f);
		if (contrast_ratio(bg, c) >= 4.5f)
			return c;
	}
	return fg;
}

/* c as it is if it has 4.5:1 on bg, else moved towards black or white just as
 * far as that needs. As pulseControl's readable(). */
static uint32_t readable(uint32_t bg, uint32_t c)
{
	uint32_t to = contrast(bg);
	for (int k = 0; k < 20; k++) {
		uint32_t m = mix(c, to, k / 20.0f);
		if (contrast_ratio(bg, m) >= 4.5f)
			return m;
	}
	return to;
}

void theme_make(struct theme *t, uint32_t bg, uint32_t act, const uint32_t *fg,
	const uint32_t *sel_fg)
{
	t->bg = bg;
	t->top = mix(bg, 0xffffff, 0.55f);
	t->bot = mix(bg, 0x000000, 0.5f);
	t->list = mix(bg, 0xffffff, 0.3f);
	t->trough = mix(bg, 0x000000, 0.12f);
	t->sel = act;
	t->mark = mix(t->list, act, 0.4f);
	t->folder = 0xc9a85c;
	t->paper = 0xf6f6f6;

	/* The user's colours as they are, as mwc takes them for its titles;
	 * else black or white by the face the text is on, which for the list
	 * is lighter than bg. */
	t->fg = fg ? *fg : contrast(bg);
	t->list_fg = fg ? *fg : contrast(t->list);
	t->mark_fg = fg ? *fg : contrast(t->mark);
	t->sel_fg = sel_fg ? *sel_fg : contrast(act);
	/* Paper is light whatever the theme: a light fg goes dark there. */
	t->paper_fg = readable(t->paper, t->fg);
	t->dim = dimmed(bg, t->fg);
	t->list_dim = dimmed(t->list, t->list_fg);
	t->mark_dim = dimmed(t->mark, t->mark_fg);
	/* by the background, not fg: readable on dark themes */
	t->err = readable(bg, contrast(bg) == 0xffffff ? 0xffa0a0 : 0x8a0303);
}
