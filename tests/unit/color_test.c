/* color_test.c - color.c: mwc's Motif shades and text, WCAG, colour names. */
#include "../../mtk.h"
#include "unit.h"

TEST(mix)
{
	CHECK_INT(mtk_mix(0xaeb2c3, 0xffffff, 0.55f), 0xdbdce4);
	CHECK_INT(mtk_mix(0xaeb2c3, 0x000000, 0.5f), 0x575962);
	CHECK_INT(mtk_mix(0x123456, 0xabcdef, 0), 0x123456);
	CHECK_INT(mtk_mix(0x123456, 0xabcdef, 1), 0xabcdef);
}

/* The values of mwc's tests/unit/draw_test.c: Motif 2.3.8's CalculateColorsRGB() as
 * mwm shows them. */
TEST(shade)
{
	struct mtk_shade s = mtk_shade(0x7387ad, 0x123456);
	CHECK_INT(s.bg, 0x7387ad);
	CHECK_INT(s.fg, 0x123456);
	/* Medium: the top towards white, the bottom towards black. */
	CHECK_INT(s.top, 0xc0c9da);
	CHECK_INT(s.bot, 0x394356);
	s = mtk_shade(0xaeb2c3, 0);
	CHECK_INT(s.top, 0xdddee6);
	CHECK_INT(s.bot, 0x5e6069);
	s = mtk_shade(0x2e5e3e, 0);
	CHECK_INT(s.top, 0x9bb2a2);
	CHECK_INT(s.bot, 0x142a1c);
	/* Light: both darker. */
	s = mtk_shade(0xf0f0f0, 0);
	CHECK_INT(s.top, 0xc0c0c0);
	CHECK_INT(s.bot, 0x909090);
	s = mtk_shade(0xffffff, 0);
	CHECK_INT(s.top, 0xcccccc);
	CHECK_INT(s.bot, 0x999999);
	/* Dark: both lighter. */
	s = mtk_shade(0x202020, 0);
	CHECK_INT(s.top, 0x909090);
	CHECK_INT(s.bot, 0x636363);
	s = mtk_shade(0x000000, 0);
	CHECK_INT(s.top, 0x7f7f7f);
	CHECK_INT(s.bot, 0x4c4c4c);
	/* The thresholds, from either side. */
	CHECK(mtk_shade(0x323232, 0).bot >> 16 > 0x32);
	CHECK(mtk_shade(0x333333, 0).bot >> 16 < 0x33);
	CHECK(mtk_shade(0xededed, 0).top >> 16 > 0xed);
	CHECK(mtk_shade(0xeeeeee, 0).top >> 16 < 0xee);
}

TEST(text_color)
{
	CHECK_INT(mtk_text_color(0xffffff), 0x000000);
	CHECK_INT(mtk_text_color(0x000000), 0xffffff);
	CHECK_INT(mtk_text_color(0xaeb2c3), 0x000000);  /* the default frame, at 71% */
	CHECK_INT(mtk_text_color(0x7387ad), 0xffffff);  /* the focused one, at 55% */
	CHECK_INT(mtk_text_color(0x8a0303), 0xffffff);
	CHECK_INT(mtk_text_color(0xffff00), 0x000000);
	CHECK_INT(mtk_text_color(0xffd700), 0xffffff);  /* gold: mwm's is white too */
	CHECK_INT(mtk_text_color(0x0000ff), 0xffffff);
	CHECK_INT(mtk_text_color(0x5f9ea0), 0xffffff);  /* CadetBlue */
	CHECK_INT(mtk_text_color(0xb2b2b2), 0xffffff);
	CHECK_INT(mtk_text_color(0xb3b3b3), 0x000000);
}

TEST(wcag)
{
	CHECK_INT(mtk_best_contrast(0xaeb2c3), 0x000000);
	CHECK_INT(mtk_best_contrast(0x7387ad), 0x000000);       /* a mid-tone: black */
	CHECK_INT(mtk_best_contrast(0x202020), 0xffffff);
	CHECK_INT(mtk_best_contrast(0x777777), 0x000000);       /* equal near 0x76 */
	CHECK_INT(mtk_best_contrast(0x737373), 0xffffff);
	CHECK(mtk_luminance(0x000000) == 0 && mtk_luminance(0xffffff) > 0.999f);
	CHECK(mtk_contrast_ratio(0, 0xffffff) > 20.99f);
	CHECK(mtk_contrast_ratio(0xffffff, 0) == mtk_contrast_ratio(0, 0xffffff));
	CHECK(mtk_contrast_ratio(0x123456, 0x123456) == 1);
}

TEST(dimmed)
{
	/* the dock tools' labels, as digitalClock's tests see them */
	CHECK_INT(mtk_dimmed(0xaeb2c3, 0x000000), 0x3d3e44);
	CHECK_INT(mtk_dimmed(0x7387ad, 0x000000), 0x171b23);
	CHECK_INT(mtk_dimmed(0x808080, 0x606060), 0x606060);    /* out of reach */
	/* 4.5:1 at half the way already, but never less than 12/20 of it */
	CHECK_INT(mtk_dimmed(0x000000, 0xffffff), 0x999999);
}

TEST(readable)
{
	CHECK_INT(mtk_readable(0xffffff, 0x000000, 0xffffff), 0x000000);       /* as is */
	uint32_t c = mtk_readable(0x404040, 0x8a0303, 0xffffff);
	CHECK(c != 0x8a0303 && mtk_contrast_ratio(0x404040, c) >= 4.5f);
	/* not further than needed: a step less falls short */
	CHECK(mtk_contrast_ratio(0x404040, mtk_mix(c, 0x8a0303, 0.06f)) < 4.5f);
	CHECK_INT(mtk_readable(0x808080, 0x808080, 0x909090), 0x909090);
}

static int color(const char *s, uint32_t *c)
{
	*c = 0xdeadbe;
	return mtk_parse_color(s, c);
}

TEST(parse_color)
{
	uint32_t c;
	CHECK(color("#ff8000", &c) && c == 0xff8000);
	CHECK(color("gold", &c) && c == 0xffd700);
	CHECK(color("CadetBlue", &c) && c == 0x5f9ea0);
	CHECK(color("rgb:f/80/ff00", &c) && c == 0xff80fe);
	CHECK(color("RGB:0/0/0", &c) && c == 0x000000);
	CHECK(color("rgb:123/fff/0", &c) && c == 0x12ff00);
	CHECK(color("rgb:aB/Cd/eF", &c) && c == 0xabcdef);
	static const char *bad[] = {
		"", "rgb:", "rgb:1/2", "rgb:1/2/3/4", "rgb:1/2/3 ", "rgb: 1/2/3",
		"rgb:+1/2/3", "rgb:0x1/2/3", "rgb:12345/1/1", "rgb:1//3", "rgb:g/0/0",
		"no such colour", "#ff80",
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		CHECK(!color(bad[i], &c));
		CHECK_INT(c, 0xdeadbe);
	}
	CHECK(!color(NULL, &c) && c == 0xdeadbe);
}

int main(void)
{
	RUN(mix);
	RUN(shade);
	RUN(text_color);
	RUN(wcag);
	RUN(dimmed);
	RUN(readable);
	RUN(parse_color);
	return unit_done();
}
