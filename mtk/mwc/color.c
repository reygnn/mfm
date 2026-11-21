/*
 * color.c: Motif's default colour calculation, CalculateColorsRGB() in lib/Xm/Color.c of
 * Motif 2.3.8, in its 16-bit integer arithmetic so the shades come out as mwm's, and with
 * its XmScreen defaults: darkThreshold 20, lightThreshold 93, foregroundThreshold 70, in
 * percent of the brightness. An 8-bit channel v is X's v * 257; a 16-bit result goes to
 * the screen as its high byte, as the X server and cairo cut it.
 */
#include "color.h"

#define XM_MAX 65535
#define XM_PERCENT (XM_MAX / 100)
#define XM_DARK (20 * XM_PERCENT)
#define XM_LIGHT (93 * XM_PERCENT)
#define XM_FOREGROUND (70 * XM_PERCENT)

static void xm_channels(uint32_t rgb, int c[3])
{
	c[0] = (int)((rgb >> 16) & 0xff) * 257;
	c[1] = (int)((rgb >> 8) & 0xff) * 257;
	c[2] = (int)(rgb & 0xff) * 257;
}

/* Brightness(): three quarters the mean of the channels, a quarter the luminosity. */
static int xm_brightness(const int c[3])
{
	int intensity = (c[0] + c[1] + c[2]) / 3;
	int luminosity = (int)(0.30 * (float)c[0] + 0.59 * (float)c[1] +
		0.11 * (float)c[2]);
	return (intensity * 75 + luminosity * 25) / 100;
}

/* f percent of the way towards white, or towards black for a negative f. */
static uint32_t xm_shift(const int c[3], int f)
{
	uint32_t out = 0;
	for (int i = 0; i < 3; i++) {
		int v = f >= 0 ? c[i] + f * (XM_MAX - c[i]) / 100 : c[i] - c[i] * -f / 100;
		out = out << 8 | (uint32_t)v >> 8;
	}
	return out;
}

uint32_t color_text(uint32_t bg)
{
	int c[3];
	xm_channels(bg, c);
	return xm_brightness(c) > XM_FOREGROUND ? 0x000000 : 0xffffff;
}

uint32_t color_inactive_text(uint32_t bg, bool builtin)
{
	return builtin ? 0x000000 : color_text(bg);
}

/*
 * By brightness:
 *   dark (below 20%):   both towards white, the top 50%, the bottom 30%;
 *   light (above 93%):  both towards black, the top 20%, the bottom 40%;
 *   medium:             the top 50 to 60% towards white, the bottom 60 to 40%
 *                       towards black, the brighter the background the more and less.
 */
struct color_shades color_shades(uint32_t bg)
{
	int c[3];
	xm_channels(bg, c);
	int b = xm_brightness(c);
	if (b < XM_DARK)
		return (struct color_shades){ xm_shift(c, 50), xm_shift(c, 30) };
	if (b > XM_LIGHT)
		return (struct color_shades){ xm_shift(c, -20), xm_shift(c, -40) };
	/* XmCOLOR_LO/HI_TS_FACTOR 50/60, _BS_FACTOR 60/40, as C divides. */
	return (struct color_shades){ xm_shift(c, 50 + b * (60 - 50) / XM_MAX),
		xm_shift(c, -(60 + b * (40 - 60) / XM_MAX)) };
}
