/* test_theme.c - every text colour readable on the face it is drawn on: make check */
#include <stdio.h>

#include "theme.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* WCAG AA for small text, on every face text is drawn on. */
static void readable_all(uint32_t bg, uint32_t act, const uint32_t *fg,
	const uint32_t *sel_fg)
{
	struct theme t;
	theme_make(&t, bg, act, fg, sel_fg);
	/* The user's own colours are taken as they are: what comes of them is
	 * theirs, and left out here. */
	const struct { const char *what; uint32_t c, face; int user; } pairs[] = {
		{ "err on bg", t.err, t.bg, 0 },
		{ "paper_fg on paper", t.paper_fg, t.paper, 0 },
		{ "fg on bg", t.fg, t.bg, !!fg },
		{ "dim on bg", t.dim, t.bg, !!fg },
		{ "list_fg on list", t.list_fg, t.list, !!fg },
		{ "list_dim on list", t.list_dim, t.list, !!fg },
		{ "mark_fg on mark", t.mark_fg, t.mark, !!fg },
		{ "mark_dim on mark", t.mark_dim, t.mark, !!fg },
		{ "sel_fg on sel", t.sel_fg, t.sel, !!sel_fg },
	};
	for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++) {
		if (pairs[i].user)
			continue;
		checks++;
		float r = contrast_ratio(pairs[i].c, pairs[i].face);
		if (r < 4.5f) {
			failures++;
			fprintf(stderr, "FAIL: bg #%06x act #%06x: %s #%06x on "
				"#%06x is %.2f:1\n", bg, act, pairs[i].what,
				pairs[i].c, pairs[i].face, r);
		}
	}
}

int main(void)
{
	/* The default look as before. */
	struct theme t;
	theme_make(&t, 0xaeb2c3, 0x7387ad, NULL, NULL);
	CHECK(t.fg == 0 && t.list_fg == 0 && t.mark_fg == 0 && t.sel_fg == 0);
	CHECK(t.paper_fg == 0 && t.err == 0x8a0303);
	CHECK(t.list == 0xc6c9d5 && t.dim == 0x3d3e44);
	/* Dark: light text, the light error red. */
	theme_make(&t, 0x202020, 0x7387ad, NULL, NULL);
	CHECK(t.fg == 0xffffff && t.err == 0xffa0a0 && t.paper_fg != 0xffffff);

	/* Every grey, with mwc's active colour and with greys for that too;
	 * #76 is where black and white swap, #008080 has white text on a list
	 * that wants black. */
	static const uint32_t act[] = { 0x7387ad, 0x000000, 0x767676, 0xffffff };
	for (uint32_t g = 0; g < 256; g++)
		for (size_t a = 0; a < sizeof act / sizeof *act; a++)
			readable_all(g * 0x010101, act[a], NULL, NULL);
	static const uint32_t hue[] = { 0x008080, 0x336699, 0xff0000, 0x00ff00,
		0x0000ff, 0x800080, 0xc9a85c, 0x5a5a5a, 0x3d3e44 };
	for (size_t h = 0; h < sizeof hue / sizeof *hue; h++)
		for (size_t a = 0; a < sizeof hue / sizeof *hue; a++)
			readable_all(hue[h], hue[a], NULL, NULL);

	/* The user's colours stay as they are where they are drawn on the
	 * theme; on paper, which is light whatever the theme, a light one
	 * goes dark. */
	uint32_t red = 0xc00000, green = 0x00c000, white = 0xffffff;
	theme_make(&t, 0xaeb2c3, 0x7387ad, &red, &green);
	CHECK(t.fg == red && t.list_fg == red && t.mark_fg == red &&
		t.sel_fg == green && t.paper_fg == red);
	for (uint32_t g = 0; g < 256; g += 5) {
		readable_all(g * 0x010101, 0x7387ad, &white, &white);
		readable_all(g * 0x010101, 0x7387ad, &red, &green);
	}

	printf("test_theme: %d/%d\n", checks - failures, checks);
	return failures != 0;
}
