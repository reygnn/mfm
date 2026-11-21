/* draw_test.c - draw.c: bevels, grooves and the border, read back from a Cairo
 * image surface as the windows' buffers are made. */
#include <stdio.h>
#include <string.h>

#include "../../mtk.h"
#include "unit.h"

#define W 12
#define H 9
#define BG 0x123456

static cairo_surface_t *cs;
static cairo_t *cr;

static void clear(void)
{
	mtk_fill(cr, BG, 0, 0, W, H);
}

static uint32_t px(int x, int y)
{
	cairo_surface_flush(cs);
	const unsigned char *d = cairo_image_surface_get_data(cs);
	int stride = cairo_image_surface_get_stride(cs);
	return *(const uint32_t *)(d + y * stride + 4 * x) & 0xffffff;
}

/* The picture as one letter per pixel, rows split by '|'. */
static void picture(char *out, uint32_t a, uint32_t b)
{
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint32_t c = px(x, y);
			*out++ = c == a ? 'a' : c == b ? 'b' : c == BG ? '.' : '?';
		}
		*out++ = '|';
	}
	*out = 0;
}

TEST(bevel)
{
	char got[(W + 1) * H + 1];
	clear();
	mtk_bevel(cr, 1, 1, 10, 7, 2, 0xaaaaaa, 0xbbbbbb);
	picture(got, 0xaaaaaa, 0xbbbbbb);
	/* Split on the diagonal at the top right and the bottom left, the pixel on it
	 * the top's. */
	CHECK(!strcmp(got,
		"............|"
		".aaaaaaaaaa.|"
		".aaaaaaaaab.|"
		".aa......bb.|"
		".aa......bb.|"
		".aa......bb.|"
		".aabbbbbbbb.|"
		".abbbbbbbbb.|"
		"............|") || (fprintf(stderr, "%s\n", got), 0));
	/* Too small for the width: no row twice, nothing outside. */
	clear();
	mtk_bevel(cr, 0, 0, 3, 3, 5, 0xaaaaaa, 0xbbbbbb);
	picture(got, 0xaaaaaa, 0xbbbbbb);
	CHECK(!strncmp(got, "aaa.........|aab.........|abb.........|", 39) ||
		(fprintf(stderr, "%s\n", got), 0));
	CHECK(!strcmp(got + 39, "............|............|............|"
		"............|............|............|"));
	clear();
	mtk_bevel(cr, 2, 2, 0, 0, 2, 0xaaaaaa, 0xbbbbbb);
	mtk_bevel(cr, 2, 2, -3, 4, 2, 0xaaaaaa, 0xbbbbbb);
	picture(got, 0xaaaaaa, 0xbbbbbb);
	CHECK(!strchr(got, 'a') && !strchr(got, 'b'));
}

TEST(shadow)
{
	struct mtk_shade s = mtk_shade(0xaeb2c3, 0);
	clear();
	mtk_shadow(cr, 0, 0, W, H, 1, &s, 0);
	CHECK_INT(px(0, 0), s.top);
	CHECK_INT(px(W - 1, H - 1), s.bot);
	CHECK_INT(px(1, 1), BG);
	mtk_shadow(cr, 0, 0, W, H, 1, &s, 1);
	CHECK_INT(px(0, 0), s.bot);
	CHECK_INT(px(W - 1, H - 1), s.top);
}

TEST(groove)
{
	struct mtk_shade s = mtk_shade(0xaeb2c3, 0);
	clear();
	mtk_groove_h(cr, 2, 3, 4, &s);
	CHECK_INT(px(2, 3), s.bot);
	CHECK_INT(px(5, 3), s.bot);
	CHECK_INT(px(2, 4), s.top);
	CHECK_INT(px(6, 3), BG);
	CHECK_INT(px(2, 5), BG);
	clear();
	mtk_groove_v(cr, 3, 1, 5, &s);
	CHECK_INT(px(3, 1), s.bot);
	CHECK_INT(px(4, 5), s.top);
	CHECK_INT(px(3, 6), BG);
	CHECK_INT(px(5, 1), BG);
}

TEST(border)
{
	struct mtk_shade s = mtk_shade(0x7387ad, 0);
	clear();
	mtk_border(cr, W, H, &s);
	/* the bevel, then a line of the face, then the inside untouched */
	CHECK_INT(px(0, 4), s.top);
	CHECK_INT(px(1, 4), s.top);
	CHECK_INT(px(2, 4), s.bg);
	CHECK_INT(px(3, 4), BG);
	CHECK_INT(px(W - 1, 4), s.bot);
	CHECK_INT(px(W - 3, 4), s.bg);
	CHECK_INT(px(W - 4, 4), BG);
	CHECK_INT(px(5, H - 2), s.bot);
	CHECK_INT(px(5, H - 3), s.bg);
	CHECK_INT(px(5, 2), s.bg);
	CHECK_INT(px(5, 3), BG);
}

/* A colour to the screen and back: what the buffer holds is what was asked. */
TEST(rgb)
{
	static const uint32_t c[] = { 0, 0xffffff, 0x010203, 0xaeb2c3, 0x7f8081 };
	for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
		mtk_fill(cr, c[i], 0, 0, 1, 1);
		CHECK_INT(px(0, 0), c[i]);
	}
	mtk_fill(cr, 0xffffff, 0, 0, 0, 5);     /* nothing for an empty one */
	mtk_fill(cr, 0xffffff, 0, 0, 5, -1);
	CHECK_INT(px(0, 0), c[4]);
}

TEST(text)
{
	PangoFontDescription *f = mtk_dock_font("draw_test");
	PangoLayout *l = mtk_layout_new(f);
	int w = mtk_text_width(l, "0123456789");
	CHECK(w > 30 && w < 200);
	CHECK_INT(mtk_text_width(l, ""), 0);
	CHECK(mtk_text_width(l, "00") < mtk_text_width(l, "0000"));
	/* Black text on white: some ink, all of it in the box but for the pixel of
	 * antialiasing around it. */
	cairo_surface_t *big = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 100, 40);
	cairo_t *c = cairo_create(big);
	mtk_fill(c, 0xffffff, 0, 0, 100, 40);
	mtk_text(c, l, "88", 0x000000, 10, 5);
	cairo_surface_flush(big);
	int ink = 0, outside = 0, stride = cairo_image_surface_get_stride(big);
	const unsigned char *d = cairo_image_surface_get_data(big);
	int tw, th;
	pango_layout_get_pixel_size(l, &tw, &th);
	for (int y = 0; y < 40; y++)
		for (int x = 0; x < 100; x++)
			if ((*(const uint32_t *)(d + y * stride + 4 * x) & 0xffffff) !=
			    0xffffff) {
				ink++;
				outside += x < 9 || y < 4 || x > 10 + tw || y > 5 + th;
			}
	CHECK(ink > 20);
	CHECK_INT(outside, 0);
	cairo_destroy(c);
	cairo_surface_destroy(big);
	g_object_unref(l);
	pango_font_description_free(f);

	/* A family fontconfig doesn't have is no font; the generic ones are. */
	CHECK(!mtk_font("draw_test", "No Such Family Anywhere 10"));
	PangoFontDescription *sans = mtk_font("draw_test", "Sans 12");
	CHECK(sans && pango_font_description_get_size(sans) == 12 * PANGO_SCALE);
	if (sans)
		pango_font_description_free(sans);
	PangoFontDescription *dv = mtk_font("draw_test", "DejaVu Sans Bold 9");
	CHECK(dv != NULL);
	if (dv)
		pango_font_description_free(dv);
}

/* Rows of 4 bytes, the ones in ch[] changed. */
static int damage(const char *ch, int gap, int max, int rows[][2])
{
	enum { ROWS = 40 };
	char a[ROWS][4] = { { 0 } }, b[ROWS][4] = { { 0 } };
	for (const char *c = ch; *c; c++)
		b[*c - '0'][1] = 1;
	return mtk_damage_rows(b, a, 4, ROWS, gap, max, rows);
}

TEST(damage)
{
	int r[4][2];
	CHECK_INT(damage("", 8, 4, r), 0);              /* nothing changed */
	CHECK_INT(damage("5", 8, 4, r), 1);
	CHECK(r[0][0] == 5 && r[0][1] == 1);
	/* A gap of 8 is no gap, one of 9 is. */
	CHECK_INT(damage("5=", 8, 4, r), 1);            /* '=' is row 13 */
	CHECK(r[0][0] == 5 && r[0][1] == 9);
	CHECK_INT(damage("5>", 8, 4, r), 2);            /* '>' is row 14 */
	CHECK(r[0][0] == 5 && r[0][1] == 1 && r[1][0] == 14 && r[1][1] == 1);
	/* At most max: the last takes in the rest. */
	CHECK_INT(damage("0;FQ", 3, 2, r), 2);          /* rows 0, 11, 22, 33 */
	CHECK(r[0][0] == 0 && r[0][1] == 1 && r[1][0] == 11 && r[1][1] == 23);
	CHECK_INT(damage("0;FQ", 3, 4, r), 4);
	CHECK(r[3][0] == 33 && r[3][1] == 1);
	/* The ends of the buffer. */
	CHECK_INT(damage("0W", 100, 4, r), 1);          /* rows 0 and 39 */
	CHECK(r[0][0] == 0 && r[0][1] == 40);
	CHECK_INT(damage("5", 8, 0, r), 0);
}

/* Pixels of colour c in columns x0..x1 of a field drawn on white. */
static int ink_in(cairo_surface_t *f, int x0, int x1, uint32_t c)
{
	cairo_surface_flush(f);
	const unsigned char *d = cairo_image_surface_get_data(f);
	int stride = cairo_image_surface_get_stride(f), n = 0;
	for (int y = 0; y < cairo_image_surface_get_height(f); y++)
		for (int x = x0; x <= x1; x++)
			n += (*(const uint32_t *)(d + y * stride + 4 * x) & 0xffffff) == c;
	return n;
}

static int field(const char *text, size_t cur, int selected, int *first, int *last)
{
	enum { FW = 300, FH = 24, BX = 50, BW = 120 };
	cairo_surface_t *f = cairo_image_surface_create(CAIRO_FORMAT_RGB24, FW, FH);
	cairo_t *c = cairo_create(f);
	cairo_set_antialias(c, CAIRO_ANTIALIAS_NONE);
	PangoFontDescription *fd = mtk_dock_font("draw_test");
	PangoLayout *l = mtk_layout_new(fd);
	mtk_fill(c, 0xffffff, 0, 0, FW, FH);
	mtk_field_draw(c, l, text, cur, selected, BX, 0, BW, FH, 0x000000, 0x7387ad,
		0xffffff);
	*first = *last = -1;
	for (int x = 0; x < FW; x++)
		if (ink_in(f, x, x, 0x000000) || ink_in(f, x, x, 0x7387ad)) {
			if (*first < 0)
				*first = x;
			*last = x;
		}
	int sel = ink_in(f, 0, FW - 1, 0x7387ad);
	g_object_unref(l);
	pango_font_description_free(fd);
	cairo_destroy(c);
	cairo_surface_destroy(f);
	return sel;
}

TEST(field_draw)
{
	enum { BX = 50, BW = 120 };
	int first, last;
	const char *lng = "a long line that has no room in the field at all, none";
	CHECK_INT(field("abc", 3, 0, &first, &last), 0);
	CHECK(first >= BX && last < BX + BW);
	CHECK(field("abc", 3, 1, &first, &last) > 20);  /* the selection */
	CHECK_INT(field("abc", 0, 1, &first, &last), 0);        /* nothing before it */
	/* Too long: in the box wherever the cursor is, the cursor shown. */
	for (size_t cur = 0; cur <= strlen(lng); cur += 11) {
		field(lng, cur, 0, &first, &last);
		CHECK(first >= BX && last < BX + BW - 1);
		CHECK(last > BX + BW / 2);      /* cut at the box's end, not short of it */
	}
	field(lng, strlen(lng), 1, &first, &last);
	CHECK(first >= BX && last < BX + BW - 1);
	/* A cursor inside a grapheme goes to its start; nothing past the box. */
	field("xe\xcc\x81y", 2, 0, &first, &last);
	CHECK(first >= BX && last < BX + BW);
	/* Too narrow for anything: nothing. */
	cairo_surface_t *f = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 40, 24);
	cairo_t *c = cairo_create(f);
	PangoLayout *l = mtk_layout_new(NULL);
	mtk_fill(c, 0xffffff, 0, 0, 40, 24);
	mtk_field_draw(c, l, "abc", 3, 0, 0, 0, 8, 24, 0, 0x7387ad, 0xffffff);
	CHECK_INT(ink_in(f, 0, 39, 0x000000), 0);
	g_object_unref(l);
	cairo_destroy(c);
	cairo_surface_destroy(f);
}

int main(void)
{
	cs = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
	cr = cairo_create(cs);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);  /* as the windows' buffers */
	RUN(bevel);
	RUN(shadow);
	RUN(groove);
	RUN(border);
	RUN(rgb);
	RUN(text);
	RUN(damage);
	RUN(field_draw);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	return unit_done();
}
