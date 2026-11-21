/* field_test.c - field.c: editing a line as mfm's fields do, with real UTF-8. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../../mtk.h"
#include "unit.h"

static char buf[16];
static struct mtk_field f;

static void set(const char *s, size_t cur, int fresh, int selects)
{
	f = (struct mtk_field){ .text = buf, .size = sizeof buf };
	mtk_field_set(&f, s);
	f.cur = cur;
	f.fresh = fresh;
	f.selects = selects;
}

static int key(xkb_keysym_t ks)
{
	return mtk_field_key(&f, ks, "", 0);
}

static int type(const char *t)
{
	return mtk_field_key(&f, XKB_KEY_a, t, 0);
}

TEST(set)
{
	f = (struct mtk_field){ .text = buf, .size = sizeof buf };
	mtk_field_set(&f, "abc");
	CHECK(!strcmp(buf, "abc") && f.cur == 3 && f.fresh);
	/* Too long: cut before a character, not inside one. */
	mtk_field_set(&f, "abcdefghijklmn\xc3\xa4z");
	CHECK_INT(strlen(buf), 14);
	CHECK_INT(f.cur, 14);
}

TEST(typing)
{
	set("ac", 1, 0, 0);
	CHECK_INT(type("b"), MTK_FIELD_CHANGED);
	CHECK(!strcmp(buf, "abc") && f.cur == 2);
	CHECK_INT(type("\xc3\xa4"), MTK_FIELD_CHANGED);
	CHECK(!strcmp(buf, "ab\xc3\xa4" "c") && f.cur == 4);
	/* Control characters, Ctrl and Alt type nothing. */
	CHECK_INT(type("\x7f"), MTK_FIELD_NONE);
	CHECK_INT(type("\t"), MTK_FIELD_NONE);
	CHECK_INT(mtk_field_key(&f, XKB_KEY_x, "x", MTK_MOD_CTRL), MTK_FIELD_NONE);
	CHECK_INT(mtk_field_key(&f, XKB_KEY_x, "x", MTK_MOD_ALT), MTK_FIELD_NONE);
	CHECK_INT(mtk_field_key(&f, XKB_KEY_X, "X", MTK_MOD_SHIFT), MTK_FIELD_CHANGED);
	/* Full: nothing goes in, not a part. */
	set("0123456789abcd", 14, 0, 0);
	CHECK_INT(type("\xc3\xa4"), MTK_FIELD_NONE);
	CHECK_INT(type("e"), MTK_FIELD_CHANGED);
	CHECK_INT(type("f"), MTK_FIELD_NONE);
	CHECK_INT(strlen(buf), 15);
}

TEST(moving)
{
	/* e with a combining acute is one step; so is a flag. */
	set("xe\xcc\x81y", 0, 0, 0);
	CHECK_INT(key(XKB_KEY_Right), MTK_FIELD_MOVED);
	CHECK_INT(f.cur, 1);
	CHECK_INT(key(XKB_KEY_Right), MTK_FIELD_MOVED);
	CHECK_INT(f.cur, 4);
	CHECK_INT(key(XKB_KEY_Left), MTK_FIELD_MOVED);
	CHECK_INT(f.cur, 1);
	CHECK_INT(key(XKB_KEY_End), MTK_FIELD_MOVED);
	CHECK_INT(f.cur, 5);
	CHECK_INT(key(XKB_KEY_End), MTK_FIELD_NONE);    /* nothing to show */
	CHECK_INT(key(XKB_KEY_Right), MTK_FIELD_NONE);
	CHECK_INT(key(XKB_KEY_Home), MTK_FIELD_MOVED);
	CHECK_INT(key(XKB_KEY_Left), MTK_FIELD_NONE);
	set("\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa!", 0, 0, 0);     /* the German flag */
	key(XKB_KEY_Right);
	CHECK_INT(f.cur, 8);
	/* A byte that is no UTF-8: a step of its own, and the rest keeps its
	 * graphemes. */
	set("\xe9" "e\xcc\x81", 0, 0, 0);
	key(XKB_KEY_Right);
	CHECK_INT(f.cur, 1);
	key(XKB_KEY_Right);
	CHECK_INT(f.cur, 4);
	key(XKB_KEY_Left);
	CHECK_INT(f.cur, 1);
}

TEST(deleting)
{
	set("ae\xcc\x81z", 4, 0, 0);
	CHECK_INT(key(XKB_KEY_BackSpace), MTK_FIELD_CHANGED);   /* the whole grapheme */
	CHECK(!strcmp(buf, "az") && f.cur == 1);
	CHECK_INT(key(XKB_KEY_Delete), MTK_FIELD_CHANGED);
	CHECK(!strcmp(buf, "a") && f.cur == 1);
	CHECK_INT(key(XKB_KEY_Delete), MTK_FIELD_NONE);
	key(XKB_KEY_Home);
	CHECK_INT(key(XKB_KEY_BackSpace), MTK_FIELD_NONE);
	CHECK_INT(mtk_field_key(&f, XKB_KEY_u, "", MTK_MOD_CTRL), MTK_FIELD_CHANGED);
	CHECK(!strcmp(buf, "") && f.cur == 0);
	CHECK_INT(mtk_field_key(&f, XKB_KEY_u, "", MTK_MOD_CTRL), MTK_FIELD_NONE);
	/* Deleting joins stray bytes into a character: the cursor stays on its
	 * start. */
	set("\xc3x\xa4", 1, 0, 0);
	CHECK_INT(key(XKB_KEY_Delete), MTK_FIELD_CHANGED);
	CHECK(!strcmp(buf, "\xc3\xa4"));
	CHECK_INT(f.cur, 2);
}

TEST(fresh)
{
	/* Fresh without selects: typing goes in at the cursor, and it is fresh no
	 * more. */
	set("/home/", 6, 1, 0);
	CHECK_INT(type("x"), MTK_FIELD_CHANGED);
	CHECK(!strcmp(buf, "/home/x") && !f.fresh);
	/* With selects the text before the cursor is replaced, the rest stays: a
	 * name without its extension. */
	set("name.txt", 4, 1, 1);
	CHECK_INT(type("new"), MTK_FIELD_CHANGED);
	CHECK(!strcmp(buf, "new.txt") && f.cur == 3 && !f.fresh);
	set("name.txt", 4, 1, 1);
	CHECK_INT(key(XKB_KEY_BackSpace), MTK_FIELD_CHANGED);
	CHECK(!strcmp(buf, ".txt") && f.cur == 0);
	set("name.txt", 4, 1, 1);
	CHECK_INT(key(XKB_KEY_Delete), MTK_FIELD_CHANGED);
	CHECK(!strcmp(buf, ".txt"));
	/* A move ends the selection, and says so even where the cursor stays. */
	set("name.txt", 8, 1, 1);
	CHECK_INT(key(XKB_KEY_End), MTK_FIELD_MOVED);
	CHECK(!f.fresh);
	/* A key that does nothing keeps it. */
	set("name.txt", 4, 1, 1);
	CHECK_INT(mtk_field_key(&f, XKB_KEY_x, "x", MTK_MOD_CTRL), MTK_FIELD_NONE);
	CHECK(f.fresh);
	/* Replacing what would not fit keeps all. */
	set("0123456789abcd", 14, 1, 1);
	CHECK_INT(type("0123456789abcdef"), MTK_FIELD_NONE);
	CHECK(!strcmp(buf, "0123456789abcd") && f.fresh);
}

TEST(enter_cancel)
{
	set("x", 1, 1, 1);
	CHECK_INT(key(XKB_KEY_Return), MTK_FIELD_ENTER);
	CHECK_INT(key(XKB_KEY_KP_Enter), MTK_FIELD_ENTER);
	CHECK_INT(key(XKB_KEY_Escape), MTK_FIELD_CANCEL);
	CHECK(!strcmp(buf, "x") && f.fresh);
}

/* pulseControl's: digits up to 100, no leading zero. */
static int percent(char *t, size_t size, void *data)
{
	for (char *p = t; *p; p++)
		if (*p < '0' || *p > '9')
			return 0;
	while (t[0] == '0' && t[1])
		memmove(t, t + 1, strlen(t));
	return atoi(t) <= 100;
}

TEST(accept)
{
	set("50", 2, 1, 1);
	f.accept = percent;
	CHECK_INT(type("7"), MTK_FIELD_CHANGED);        /* replaces the selection */
	CHECK(!strcmp(buf, "7") && f.cur == 1);
	CHECK_INT(type("x"), MTK_FIELD_NONE);
	CHECK_INT(type("5"), MTK_FIELD_CHANGED);
	CHECK_INT(type("0"), MTK_FIELD_NONE);           /* 750 */
	CHECK(!strcmp(buf, "75"));
	key(XKB_KEY_BackSpace);
	key(XKB_KEY_BackSpace);
	CHECK_INT(type("0"), MTK_FIELD_CHANGED);
	CHECK_INT(type("5"), MTK_FIELD_CHANGED);        /* "05" is 5 */
	CHECK(!strcmp(buf, "5") && f.cur == 1);
	CHECK_INT(type("0"), MTK_FIELD_CHANGED);
	CHECK_INT(type("0"), MTK_FIELD_NONE);           /* 500 */
	CHECK(!strcmp(buf, "50") && f.cur == 2);
}

int main(void)
{
	RUN(set);
	RUN(typing);
	RUN(moving);
	RUN(deleting);
	RUN(fresh);
	RUN(enter_cancel);
	RUN(accept);
	return unit_done();
}
