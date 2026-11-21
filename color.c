/* color.c - colours by mwc's rules (its src/draw.c), WCAG contrast, colour names. */
#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mtk.h"

uint32_t mtk_mix(uint32_t rgb, uint32_t to, float f)
{
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float c = (rgb >> sh) & 0xff, t = (to >> sh) & 0xff;
		out |= (uint32_t)(c + (t - c) * f + 0.5f) << sh;
	}
	return out;
}

/*
 * Motif's CalculateColorsRGB() in lib/Xm/Color.c of Motif 2.3.8, in its 16-bit
 * integer arithmetic, with the XmScreen defaults darkThreshold 20, lightThreshold
 * 93 and foregroundThreshold 70, in percent of the brightness. An 8-bit channel is
 * c * 257 in 16 bits, as mwc's float makes it.
 */
#define XM_MAX 65535
#define XM_PERCENT (XM_MAX / 100)
#define XM_DARK (20 * XM_PERCENT)
#define XM_LIGHT (93 * XM_PERCENT)
#define XM_FOREGROUND (70 * XM_PERCENT)

static void xm_channels(uint32_t rgb, int c[3])
{
	c[0] = ((rgb >> 16) & 0xff) * 257;
	c[1] = ((rgb >> 8) & 0xff) * 257;
	c[2] = (rgb & 0xff) * 257;
}

/* The colour as the screen shows it: each 16-bit channel cut to its high byte. */
static uint32_t rgb8(const uint16_t c[3])
{
	return (uint32_t)(c[0] >> 8) << 16 | (uint32_t)(c[1] >> 8) << 8 | (c[2] >> 8);
}

/* Brightness(): three quarters the mean of the channels, a quarter the luminosity. */
static int xm_brightness(const int c[3])
{
	int intensity = (c[0] + c[1] + c[2]) / 3;
	int luminosity = (int)(0.30 * (float)c[0] + 0.59 * (float)c[1] +
		0.11 * (float)c[2]);
	return (intensity * 75 + luminosity * 25) / 100;
}

/* f percent of the way towards white, or towards black for a negative f, cut to
 * the high byte as the X server and Cairo show 16 bits. */
static uint32_t xm_shift(const int c[3], int f)
{
	uint32_t out = 0;
	for (int i = 0; i < 3; i++) {
		int v = f >= 0 ? c[i] + f * (XM_MAX - c[i]) / 100 : c[i] - c[i] * -f / 100;
		out = out << 8 | (uint32_t)(v >> 8);
	}
	return out;
}

struct mtk_shade mtk_shade(uint32_t bg, uint32_t fg)
{
	struct mtk_shade s = { .bg = bg, .fg = fg };
	int c[3];
	xm_channels(bg, c);
	int b = xm_brightness(c);
	if (b < XM_DARK) {
		s.top = xm_shift(c, 50);
		s.bot = xm_shift(c, 30);
	} else if (b > XM_LIGHT) {
		s.top = xm_shift(c, -20);
		s.bot = xm_shift(c, -40);
	} else {
		/* XmCOLOR_LO/HI_TS_FACTOR 50/60, _BS_FACTOR 60/40, as C divides. */
		s.top = xm_shift(c, 50 + b * (60 - 50) / XM_MAX);
		s.bot = xm_shift(c, -(60 + b * (40 - 60) / XM_MAX));
	}
	return s;
}

uint32_t mtk_text_color(uint32_t bg)
{
	int c[3];
	xm_channels(bg, c);
	return xm_brightness(c) > XM_FOREGROUND ? 0x000000 : 0xffffff;
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
