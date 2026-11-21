/* test_theme.c - every text colour readable on the face it is drawn on: make check */
#include <stdio.h>

#include "theme.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* WCAG AA for small text on mfm's own faces. On mwc's, bg and sel, the text is
 * as Motif picks it, and the dimmed text reads at least as well as that. */
static void readable_all(uint32_t bg, uint32_t act, const uint32_t *fg,
	const uint32_t *sel_fg)
{
	struct theme t;
	theme_make(&t, bg, act, fg, sel_fg);
	/* The user's own colours are taken as they are on their own face: what
	 * comes of them there is theirs, and left out here. */
	float fg_r = mtk_contrast_ratio(t.fg, t.bg);
	float need = fg_r < 4.5f ? fg_r : 4.5f;
	if (!fg) {
		checks++;
		if (t.fg != mtk_text_color(bg))
			failures++, fprintf(stderr, "FAIL: fg on #%06x not Motif's\n", bg);
	}
	if (!sel_fg) {
		checks++;
		if (t.sel_fg != mtk_text_color(act))
			failures++, fprintf(stderr, "FAIL: sel_fg on #%06x not Motif's\n",
				act);
	}
	const struct { const char *what; uint32_t c, face; int user; float need; }
	pairs[] = {
		{ "err on bg", t.err, t.bg, 0, 4.5f },
		{ "paper_fg on paper", t.paper_fg, t.paper, 0, 4.5f },
		{ "dim on bg", t.dim, t.bg, !!fg, need },
		{ "list_fg on list", t.list_fg, t.list, 0, 4.5f },
		{ "list_dim on list", t.list_dim, t.list, 0, 4.5f },
		{ "mark_fg on mark", t.mark_fg, t.mark, 0, 4.5f },
		{ "mark_dim on mark", t.mark_dim, t.mark, 0, 4.5f },
	};
	for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++) {
		if (pairs[i].user)
			continue;
		checks++;
		float r = mtk_contrast_ratio(pairs[i].c, pairs[i].face);
		if (r < pairs[i].need) {
			failures++;
			fprintf(stderr, "FAIL: bg #%06x act #%06x: %s #%06x on "
				"#%06x is %.2f:1\n", bg, act, pairs[i].what,
				pairs[i].c, pairs[i].face, r);
		}
	}
}

int main(void)
{
	/* The default look: Motif's shadows, white on the active colour as mwc. */
	struct theme t;
	theme_make(&t, 0xaeb2c3, 0x7387ad, NULL, NULL);
	CHECK(t.fg == 0 && t.list_fg == 0 && t.mark_fg == 0 && t.sel_fg == 0xffffff);
	CHECK(t.top == 0xdddee6 && t.bot == 0x5e6069);
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

	/* The user's colours stay as they are on their own face, and where
	 * they read; on another face they are moved as far as they need. */
	uint32_t red = 0xc00000, green = 0x00c000, white = 0xffffff;
	uint32_t grey = 0xa0a0a0, dark = 0x202020;
	theme_make(&t, 0xaeb2c3, 0x7387ad, &grey, &green);
	CHECK(t.fg == grey && t.sel_fg == green);
	CHECK(t.list_fg != grey && mtk_contrast_ratio(t.list_fg, t.list) >= 4.5f);
	CHECK(t.mark_fg != grey && mtk_contrast_ratio(t.mark_fg, t.mark) >= 4.5f);
	/* moved no further than it needs: the step before falls short */
	int k = 1;
	while (k < 20 && mtk_mix(grey, mtk_best_contrast(t.list), k / 20.0f) != t.list_fg)
		k++;
	CHECK(k < 20 && mtk_contrast_ratio(mtk_mix(grey, mtk_best_contrast(t.list),
		(k - 1) / 20.0f), t.list) < 4.5f);
	theme_make(&t, 0xaeb2c3, 0x7387ad, &dark, &green);
	CHECK(t.fg == dark && t.list_fg == dark && t.mark_fg == dark &&
		t.paper_fg == dark);
	for (uint32_t g = 0; g < 256; g += 5) {
		readable_all(g * 0x010101, 0x7387ad, &white, &white);
		readable_all(g * 0x010101, 0x7387ad, &red, &green);
	}

	printf("test_theme: %d/%d\n", checks - failures, checks);
	return failures != 0;
}
