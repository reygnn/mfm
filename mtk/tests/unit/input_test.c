/* input_test.c - input.c: keys through a real xkbcommon keymap and compose table,
 * key repeat, seats, cursor sizes. */
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../../mtk.h"
#include "unit.h"

/* evdev codes + 8 */
#define KC_ESC 9
#define KC_Q 24
#define KC_E 26
#define KC_A 38
#define KC_SHIFT 50
#define KC_CTRL 37
#define KC_ALT 64
#define KC_LEFT 113

static struct xkb_context *ctx;
static struct xkb_keymap *map;
static struct xkb_state *st;

static struct xkb_keymap *keymap(const char *layout, const char *variant)
{
	struct xkb_rule_names names = { .rules = "evdev", .model = "pc105",
		.layout = layout, .variant = variant };
	return xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
}

static void use(const char *layout, const char *variant)
{
	xkb_state_unref(st);
	xkb_keymap_unref(map);
	map = keymap(layout, variant);
	st = map ? xkb_state_new(map) : NULL;
}

static void hold(uint32_t kc, int down)
{
	xkb_state_update_key(st, kc, down ? XKB_KEY_DOWN : XKB_KEY_UP);
}

TEST(mods)
{
	use("us", NULL);
	CHECK(st != NULL);
	if (!st)
		return;
	CHECK_INT(mtk_mods(st), 0);
	hold(KC_SHIFT, 1);
	hold(KC_CTRL, 1);
	CHECK_INT(mtk_mods(st), MTK_MOD_SHIFT | MTK_MOD_CTRL);
	hold(KC_SHIFT, 0);
	hold(KC_CTRL, 0);
	hold(KC_ALT, 1);
	CHECK_INT(mtk_mods(st), MTK_MOD_ALT);
	hold(KC_ALT, 0);
	CHECK_INT(mtk_mods(NULL), 0);
}

TEST(translate)
{
	use("us", NULL);
	if (!st)
		return;
	xkb_keysym_t ks;
	char t[64];
	CHECK(mtk_key_translate(st, NULL, KC_A, &ks, t));
	CHECK_INT(ks, XKB_KEY_a);
	CHECK(!strcmp(t, "a"));
	hold(KC_SHIFT, 1);
	CHECK(mtk_key_translate(st, NULL, KC_A, &ks, t) && ks == XKB_KEY_A);
	CHECK(!strcmp(t, "A"));
	hold(KC_SHIFT, 0);
	CHECK(mtk_key_translate(st, NULL, KC_LEFT, &ks, t) && ks == XKB_KEY_Left);
	CHECK(!strcmp(t, ""));
	CHECK(mtk_key_translate(st, NULL, KC_ESC, &ks, t) && ks == XKB_KEY_Escape);
}

/* A dead key and the letter after it, as the locale's compose table makes them. */
TEST(compose)
{
	setenv("LC_ALL", "C.UTF-8", 1);
	struct xkb_compose_state *cs = mtk_compose_new(ctx);
	use("us", "intl");      /* ' is dead_acute */
	CHECK(cs != NULL && st != NULL);
	if (!cs || !st) {
		xkb_compose_state_unref(cs);
		return;
	}
	xkb_keysym_t ks;
	char t[64];
	enum { KC_APOS = 48 };
	CHECK(!mtk_key_translate(st, cs, KC_APOS, &ks, t));     /* only the sequence */
	CHECK(mtk_key_translate(st, cs, KC_E, &ks, t));
	CHECK_INT(ks, XKB_KEY_eacute);
	CHECK(!strcmp(t, "\xc3\xa9"));
	/* A letter that fits no sequence ends it and is dropped; Escape is not. */
	CHECK(!mtk_key_translate(st, cs, KC_APOS, &ks, t));
	CHECK(!mtk_key_translate(st, cs, KC_Q, &ks, t));
	CHECK(mtk_key_translate(st, cs, KC_A, &ks, t) && !strcmp(t, "a"));
	CHECK(!mtk_key_translate(st, cs, KC_APOS, &ks, t));
	CHECK(mtk_key_translate(st, cs, KC_ESC, &ks, t) && ks == XKB_KEY_Escape);
	xkb_compose_state_unref(cs);
	/* A locale without a table: none, and keys still work without one. */
	setenv("LC_ALL", "xx_NOWHERE.nothing", 1);
	cs = mtk_compose_new(ctx);
	xkb_compose_state_unref(cs);
	unsetenv("LC_ALL");
}

/* Ctrl+U in a Russian layout is the key that is U in the US one. */
TEST(latin)
{
	struct xkb_rule_names names = { .rules = "evdev", .model = "pc105",
		.layout = "ru,us" };
	struct xkb_keymap *ru = xkb_keymap_new_from_names(ctx, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	CHECK(ru != NULL);
	if (!ru)
		return;
	enum { KC_U = 30 };
	struct xkb_state *s = xkb_state_new(ru);
	xkb_keysym_t ks = xkb_state_key_get_one_sym(s, KC_U);
	CHECK(ks == XKB_KEY_Cyrillic_ghe);
	CHECK_INT(mtk_latin_sym(ru, KC_U, ks), XKB_KEY_u);
	CHECK_INT(mtk_latin_sym(ru, KC_U, XKB_KEY_U), XKB_KEY_U);       /* Latin as is */
	CHECK_INT(mtk_latin_sym(NULL, KC_U, ks), ks);
	/* A key no layout has a Latin one for keeps its own. */
	CHECK_INT(mtk_latin_sym(ru, KC_LEFT, XKB_KEY_Left), XKB_KEY_Left);
	xkb_state_unref(s);
	xkb_keymap_unref(ru);
	/* AZERTY: the "&" key is 1 for a shortcut, "é" is 2, "a" stays a. */
	names.layout = "fr";
	struct xkb_keymap *fr = xkb_keymap_new_from_names(ctx, &names,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	CHECK(fr != NULL);
	if (fr) {
		enum { KC_1 = 10, KC_2 = 11, KC_Q_POS = 24 };
		CHECK_INT(mtk_latin_sym(fr, KC_1, XKB_KEY_ampersand), XKB_KEY_1);
		CHECK_INT(mtk_latin_sym(fr, KC_2, XKB_KEY_eacute), XKB_KEY_2);
		CHECK_INT(mtk_latin_sym(fr, KC_Q_POS, XKB_KEY_a), XKB_KEY_a);
		CHECK_INT(mtk_latin_sym(fr, KC_SHIFT, XKB_KEY_Shift_L), XKB_KEY_Shift_L);
		xkb_keymap_unref(fr);
	}
	/* Without a Latin layout at all: ks. */
	names.layout = "ru";
	ru = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	CHECK(ru != NULL);
	if (ru) {
		CHECK_INT(mtk_latin_sym(ru, KC_U, XKB_KEY_Cyrillic_ghe), XKB_KEY_Cyrillic_ghe);
		xkb_keymap_unref(ru);
	}
}

TEST(quits)
{
	CHECK(mtk_key_quits(XKB_KEY_Escape, 0));
	CHECK(mtk_key_quits(XKB_KEY_q, 0));
	CHECK(mtk_key_quits(XKB_KEY_Q, MTK_MOD_SHIFT));
	CHECK(!mtk_key_quits(XKB_KEY_w, 0));
	CHECK(mtk_key_quits(XKB_KEY_w, MTK_MOD_CTRL));
	CHECK(mtk_key_quits(XKB_KEY_W, MTK_MOD_CTRL | MTK_MOD_SHIFT));
	CHECK(!mtk_key_quits(XKB_KEY_Escape, MTK_MOD_CTRL));
	CHECK(!mtk_key_quits(XKB_KEY_q, MTK_MOD_ALT));
	CHECK(!mtk_key_quits(XKB_KEY_q, MTK_MOD_LOGO));
	CHECK(!mtk_key_quits(XKB_KEY_w, MTK_MOD_CTRL | MTK_MOD_ALT));
	CHECK(!mtk_key_quits(XKB_KEY_a, 0));
}

TEST(repeat)
{
	struct mtk_repeat r;
	mtk_repeat_init(&r);
	CHECK_INT(mtk_repeat_timeout(&r, 0), -1);
	mtk_repeat_info(&r, 10, 300);           /* every 100 ms after 300 */
	mtk_repeat_press(&r, KC_A, 1, 1, 1000);
	CHECK_INT(mtk_repeat_timeout(&r, 1000), 300);
	CHECK_INT(mtk_repeat_due(&r, 1299), 0);
	CHECK_INT(mtk_repeat_due(&r, 1300), KC_A);
	CHECK_INT(mtk_repeat_timeout(&r, 1300), 100);
	CHECK_INT(mtk_repeat_due(&r, 1350), 0);
	CHECK_INT(mtk_repeat_timeout(&r, 1500), 0);     /* late: at once */
	/* A modifier doesn't stop it, another key does; one that may not repeat
	 * stops it too. */
	mtk_repeat_press(&r, KC_SHIFT, 0, 1, 1400);
	CHECK_INT(r.kc, KC_A);
	mtk_repeat_release(&r, KC_SHIFT);
	CHECK_INT(r.kc, KC_A);
	mtk_repeat_press(&r, KC_E, 1, 0, 1400);
	CHECK_INT(r.kc, 0);
	mtk_repeat_press(&r, KC_E, 1, 1, 1400);
	mtk_repeat_release(&r, KC_A);           /* the one before: no matter */
	CHECK_INT(r.kc, KC_E);
	mtk_repeat_release(&r, KC_E);
	CHECK_INT(mtk_repeat_timeout(&r, 1400), -1);
	/* Rate 0: none, also for the key repeating now. */
	mtk_repeat_press(&r, KC_A, 1, 1, 2000);
	mtk_repeat_info(&r, 0, 300);
	CHECK_INT(r.kc, 0);
	mtk_repeat_press(&r, KC_A, 1, 1, 2000);
	CHECK_INT(r.kc, 0);
	/* A rate above 1000 is every ms, not never. */
	mtk_repeat_info(&r, 5000, 0);
	mtk_repeat_press(&r, KC_A, 1, 1, 3000);
	CHECK_INT(mtk_repeat_due(&r, 3000), KC_A);
	CHECK_INT(mtk_repeat_timeout(&r, 3000), 1);
	mtk_repeat_info(&r, 1, -5);             /* no negative delay */
	mtk_repeat_press(&r, KC_A, 1, 1, 4000);
	CHECK_INT(mtk_repeat_timeout(&r, 4000), 0);
	mtk_repeat_info(&r, 1, INT_MAX);
	mtk_repeat_press(&r, KC_A, 1, 1, 0);
	CHECK_INT(mtk_repeat_timeout(&r, 0), INT_MAX);
	mtk_repeat_stop(&r);
	CHECK_INT(mtk_repeat_due(&r, UINT64_MAX), 0);
}

TEST(seats)
{
	struct mtk_seats s = { 0 };
	unsigned name, ver;
	CHECK(!mtk_seats_next(&s, &name, &ver));
	for (unsigned i = 0; i < MTK_MAX_SEATS; i++)
		CHECK(mtk_seats_add(&s, 10 + i, i));
	CHECK(!mtk_seats_add(&s, 99, 1));               /* full */
	mtk_seats_remove(&s, 10);
	CHECK(mtk_seats_next(&s, &name, &ver) && name == 11 && ver == 1);
	mtk_seats_remove(&s, 12345);                    /* not a seat */
	CHECK_INT(s.n, MTK_MAX_SEATS - 1);
	mtk_seats_remove(&s, 13);
	CHECK_INT(s.name[1], 12);
	CHECK_INT(s.name[2], 14);
	CHECK(mtk_seats_add(&s, 99, 7));
	CHECK_INT(s.name[s.n - 1], 99);
}

TEST(cursor)
{
	CHECK_INT(mtk_cursor_size(NULL), 24);
	CHECK_INT(mtk_cursor_size(""), 24);
	CHECK_INT(mtk_cursor_size("32"), 32);
	CHECK_INT(mtk_cursor_size("32px"), 24);
	CHECK_INT(mtk_cursor_size("0"), 24);
	CHECK_INT(mtk_cursor_size("-8"), 24);
	CHECK_INT(mtk_cursor_size("1000"), 256);
	CHECK_INT(mtk_cursor_size("99999999999999999999999"), 256);
	int bs, vw, vh;
	mtk_cursor_fit(48, 48, 2, 0, &bs, &vw, &vh);
	CHECK(bs == 2 && vw == -1 && vh == -1);
	/* Adwaita's 30 for 28 at scale 4: a viewport at 8, else buffer scale 3 */
	mtk_cursor_fit(30, 30, 4, 1, &bs, &vw, &vh);
	CHECK(bs == 1 && vw == 8 && vh == 8);
	mtk_cursor_fit(30, 30, 4, 0, &bs, &vw, &vh);
	CHECK(bs == 3 && vw == -1);
	mtk_cursor_fit(31, 31, 4, 0, &bs, &vw, &vh);
	CHECK(bs == 1);
	mtk_cursor_fit(1, 1, 16, 1, &bs, &vw, &vh);
	CHECK(vw == 1 && vh == 1);
	CHECK_INT(mtk_buffer_scale(0), 0);
	CHECK_INT(mtk_buffer_scale(-3), 0);
	CHECK_INT(mtk_buffer_scale(2), 2);
	CHECK_INT(mtk_buffer_scale(100), 16);
	/* Past what Cairo draws: the largest scale that fits, at least 1. */
	CHECK_INT(mtk_draw_scale(9000, 120, 4), 3);
	CHECK_INT(mtk_draw_scale(8191, 120, 4), 4);
	CHECK_INT(mtk_draw_scale(8192, 120, 4), 3);
	CHECK_INT(mtk_draw_scale(120, 20000, 2), 1);
	CHECK_INT(mtk_draw_scale(40000, 10, 3), 1);
	CHECK_INT(mtk_draw_scale(300, 120, 16), 16);
	/* A pool holds less than 2 GiB: 8192 x 8192 at 3 would be 2.4 GiB. */
	CHECK_INT(mtk_draw_scale(8192, 8192, 3), 2);
	CHECK_INT(mtk_draw_scale(8192, 8192, 2), 2);
}

TEST(scroll)
{
	struct mtk_scroll s = { 0 };
	CHECK_INT(mtk_scroll_frame(&s), 0);
	/* A wheel: its steps, the distance with them dropped. */
	mtk_scroll_value(&s, 15);
	mtk_scroll_discrete(&s, 1);
	CHECK_INT(mtk_scroll_frame(&s), 1);
	CHECK(s.acc == 0);
	CHECK_INT(mtk_scroll_frame(&s), 0);
	mtk_scroll_discrete(&s, -2);
	mtk_scroll_discrete(&s, -1);
	CHECK_INT(mtk_scroll_frame(&s), -3);
	/* A touchpad: 15 pixels a step, the rest kept for the next frame. */
	mtk_scroll_value(&s, 10);
	CHECK_INT(mtk_scroll_frame(&s), 0);
	mtk_scroll_value(&s, 10);
	CHECK_INT(mtk_scroll_frame(&s), 1);
	CHECK(s.acc == 5);
	mtk_scroll_value(&s, -40);
	CHECK_INT(mtk_scroll_frame(&s), -2);
	CHECK(s.acc == -5);
	/* The finger lifted: the rest goes. */
	mtk_scroll_value(&s, 12);
	mtk_scroll_stop(&s);
	mtk_scroll_value(&s, 12);
	CHECK_INT(mtk_scroll_frame(&s), 0);
	/* A flood: at most MTK_SCROLL_MAX, and nothing carried over. */
	mtk_scroll_discrete(&s, 1000);
	mtk_scroll_discrete(&s, 1000);
	CHECK_INT(mtk_scroll_frame(&s), MTK_SCROLL_MAX);
	mtk_scroll_discrete(&s, INT_MIN);
	CHECK_INT(mtk_scroll_frame(&s), -MTK_SCROLL_MAX);
	mtk_scroll_value(&s, -1e6);
	CHECK_INT(mtk_scroll_frame(&s), -MTK_SCROLL_MAX);
	CHECK(s.acc == 0);
	mtk_scroll_value(&s, 1e308);
	mtk_scroll_value(&s, 1e308);    /* inf */
	CHECK_INT(mtk_scroll_frame(&s), 0);
	CHECK(s.acc == 0);
}

TEST(configure_size)
{
	int fw = 300, fh = 120, w, h;
	/* 0: the client's own, the free size. */
	mtk_configure_size(0, 0, 0, 200, 100, &fw, &fh, &w, &h);
	CHECK(w == 300 && h == 120);
	/* Resized by hand: that is the free size now. */
	mtk_configure_size(400, 150, MTK_STATE_ACTIVATED, 200, 100, &fw, &fh, &w, &h);
	CHECK(w == 400 && h == 150 && fw == 400 && fh == 150);
	/* Maximized: the compositor's, the free size kept for later. */
	mtk_configure_size(1280, 720, MTK_STATE_MAXIMIZED, 200, 100, &fw, &fh, &w, &h);
	CHECK(w == 1280 && h == 720 && fw == 400 && fh == 150);
	mtk_configure_size(640, 720, MTK_STATE_TILED, 200, 100, &fw, &fh, &w, &h);
	CHECK(w == 640 && fw == 400);
	mtk_configure_size(1280, 720, MTK_STATE_FULLSCREEN, 200, 100, &fw, &fh, &w, &h);
	CHECK(w == 1280 && fw == 400);
	/* And back: 0 x 0 gives the free size again, not the maximized one. */
	mtk_configure_size(0, 0, 0, 200, 100, &fw, &fh, &w, &h);
	CHECK(w == 400 && h == 150);
	/* Never below the minimum nor above MTK_MAX_SIZE, one side at a time. */
	mtk_configure_size(50, 0, 0, 200, 100, &fw, &fh, &w, &h);
	CHECK(w == 200 && h == 150);
	mtk_configure_size(100000, 30, 0, 200, 100, &fw, &fh, &w, &h);
	CHECK(w == MTK_MAX_SIZE && h == 100);
	mtk_configure_size(-5, -5, 0, 20000, 0, &fw, &fh, &w, &h);
	CHECK(w == MTK_MAX_SIZE && h == 100);
}

int main(void)
{
	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ctx)
		xkb_context_set_log_level(ctx, XKB_LOG_LEVEL_CRITICAL);
	if (!ctx) {
		fprintf(stderr, "no xkb context\n");
		return 1;
	}
	RUN(mods);
	RUN(translate);
	RUN(compose);
	RUN(latin);
	RUN(quits);
	RUN(repeat);
	RUN(seats);
	RUN(scroll);
	RUN(configure_size);
	RUN(cursor);
	xkb_state_unref(st);
	xkb_keymap_unref(map);
	xkb_context_unref(ctx);
	return unit_done();
}
