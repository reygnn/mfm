/* color.c - colours by mwc's rules (its src/color.c, in mwc/), WCAG contrast, colour
 * names. */
#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mtk.h"
#include "mwc_color.h"
#include "mwc/color.c"          /* compiled here, with mwc_color.h's names */

uint32_t mtk_mix(uint32_t rgb, uint32_t to, float f)
{
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float c = (rgb >> sh) & 0xff, t = (to >> sh) & 0xff;
		out |= (uint32_t)(c + (t - c) * f + 0.5f) << sh;
	}
	return out;
}

/* The colour as the screen shows it: each 16-bit channel cut to its high byte. */
static uint32_t rgb8(const uint16_t c[3])
{
	return (uint32_t)(c[0] >> 8) << 16 | (uint32_t)(c[1] >> 8) << 8 | (c[2] >> 8);
}

/* Motif's shadows and text as mwc has them: a wrapper with mtk's names. */
struct mtk_shade mtk_shade(uint32_t bg, uint32_t fg)
{
	struct color_shades c = color_shades(bg);
	return (struct mtk_shade){ .bg = bg, .fg = fg, .top = c.top, .bot = c.bottom };
}

uint32_t mtk_text_color(uint32_t bg)
{
	return color_text(bg);
}

/* An sRGB channel as linear light. */
static float linear(unsigned c)
{
	float v = c / 255.0f;
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

float mtk_luminance(uint32_t c)
{
	return 0.2126f * linear((c >> 16) & 0xff) + 0.7152f * linear((c >> 8) & 0xff) +
		0.0722f * linear(c & 0xff);
}

float mtk_contrast_ratio(uint32_t a, uint32_t b)
{
	float la = mtk_luminance(a) + 0.05f, lb = mtk_luminance(b) + 0.05f;
	return la > lb ? la / lb : lb / la;
}

/* The two are equal at a relative luminance of sqrt(1.05 * 0.05) - 0.05, a grey
 * of about 0x76. */
uint32_t mtk_best_contrast(uint32_t bg)
{
	float l = mtk_luminance(bg);
	return (l + 0.05f) * (l + 0.05f) > 1.05f * 0.05f ? 0x000000 : 0xffffff;
}

uint32_t mtk_dimmed(uint32_t bg, uint32_t fg)
{
	for (int k = 12; k < 20; k++) {
		uint32_t c = mtk_mix(bg, fg, k / 20.0f);
		if (mtk_contrast_ratio(bg, c) >= 4.5f)
			return c;
	}
	return fg;
}

uint32_t mtk_readable(uint32_t bg, uint32_t c, uint32_t to)
{
	for (int k = 0; k < 20; k++) {
		uint32_t m = mtk_mix(c, to, k / 20.0f);
		if (mtk_contrast_ratio(bg, m) >= 4.5f)
			return m;
	}
	return to;
}

/* As mwc's config_color() and rgb_of(): a float of the channel, rounded to 16
 * bits. */
static uint16_t channel16(float v)
{
	return (uint16_t)lroundf(fminf(fmaxf(v, 0.0f), 1.0f) * 65535);
}

static int parse16(const char *s, uint16_t out[3])
{
	if (!s || !*s)
		return 0;
	if (!strncasecmp(s, "rgb:", 4)) {
		uint16_t c[3];
		s += 4;
		for (int i = 0; i < 3; i++) {
			/* Only hex digits, as mwc: strtoul alone also takes spaces,
			 * signs and 0x. */
			int digits = strspn(s, "0123456789abcdefABCDEF");
			if (digits < 1 || digits > 4 || s[digits] != (i < 2 ? '/' : '\0'))
				return 0;
			c[i] = channel16(strtoul(s, NULL, 16) /
				(float)((1ul << (4 * digits)) - 1));
			s += digits + 1;
		}
		memcpy(out, c, sizeof c);
		return 1;
	}
	PangoColor pc;
	if (!pango_color_parse(&pc, s))
		return 0;
	out[0] = channel16(pc.red / 65535.0f);
	out[1] = channel16(pc.green / 65535.0f);
	out[2] = channel16(pc.blue / 65535.0f);
	return 1;
}

int mtk_parse_color(const char *s, uint32_t *out)
{
	uint16_t c[3];
	if (!parse16(s, c))
		return 0;
	*out = rgb8(c);
	return 1;
}
