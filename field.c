/* field.c - a line of text to edit, without Wayland: from mfm's line_key(). */
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "mtk.h"

/* One UTF-8 character at s: its length, 0 if it is none. */
static int u8_len(const unsigned char *s, uint32_t *cp)
{
	unsigned c = s[0];
	uint32_t v, min;
	int n;
	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xc2 && c <= 0xdf) {
		n = 2; v = c & 0x1f; min = 0x80;
	} else if (c >= 0xe0 && c <= 0xef) {
		n = 3; v = c & 0x0f; min = 0x800;
	} else if (c >= 0xf0 && c <= 0xf4) {
		n = 4; v = c & 0x07; min = 0x10000;
	} else {
		return 0;
	}
	for (int i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80)      /* also stops at the NUL */
			return 0;
		v = v << 6 | (s[i] & 0x3f);
	}
	if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
		return 0;
	*cp = v;
	return n;
}

/* The next code point; a byte that is not UTF-8 counts as Latin-1, as file names
 * from old disks and network shares often are. */
static uint32_t u8_next(const char **s)
{
	uint32_t cp;
	int n = u8_len((const unsigned char *)*s, &cp);
	if (!n) {
		cp = (unsigned char)**s;
		n = 1;
	}
	*s += n;
	return cp;
}

/* The start of the last character in s[0..len). */
static size_t u8_prev(const char *s, size_t len)
{
	const char *p = s, *last = s;
	while ((size_t)(p - s) < len) {
		last = p;
		u8_next(&p);
	}
	return last - s;
}

static int is_combining(uint32_t c)
{
	return c >= 0x300 && (g_unichar_ismark(c) || c == 0x200d ||
		(c >= 0x1f3fb && c <= 0x1f3ff));
}

size_t mtk_grapheme_step(const char *b, size_t c, int dir)
{
	size_t len = strlen(b);
	const char *p;
	if (dir < 0 ? c == 0 : c >= len)
		return c;
	/* The valid UTF-8 around c, between bytes that are not: a Latin-1 byte
	 * elsewhere must not cost the rest its graphemes. */
	size_t lo = 0, hi = len;
	for (size_t i = 0; i < len;) {
		uint32_t cp;
		int l = u8_len((const unsigned char *)b + i, &cp);
		if (!l) {
			if (i < c) {
				lo = i + 1;
			} else {
				hi = i;
				break;
			}
			l = 1;
		}
		i += l;
	}
	if (dir < 0 ? c == lo : c == hi) {
		if (dir < 0) {
			while (c) {
				c = u8_prev(b, c);
				p = b + c;
				if (!is_combining(u8_next(&p)))
					break;
			}
			return c;
		}
		p = b + c;
		u8_next(&p);
		for (const char *q = p; *q && is_combining(u8_next(&q));)
			p = q;
		return p - b;
	}
	const char *r = b + lo;
	glong nch = g_utf8_strlen(r, hi - lo);
	PangoLogAttr *a = g_new(PangoLogAttr, nch + 1);
	pango_get_log_attrs(r, hi - lo, -1, pango_language_get_default(), a, nch + 1);
	glong i = g_utf8_pointer_to_offset(r, b + c);
	do
		i += dir < 0 ? -1 : 1;
	while (i > 0 && i < nch && !a[i].is_cursor_position);
	g_free(a);
	return g_utf8_offset_to_pointer(r, i) - b;
}

void mtk_field_set(struct mtk_field *f, const char *s)
{
	size_t n = strlen(s);
	if (n >= f->size)
		n = u8_prev(s, f->size);      /* cut before a character */
	memmove(f->text, s, n);
	f->text[n] = 0;
	f->cur = n;
	f->fresh = 1;
}

/* What the filter makes of t, which has room for size bytes; 0 if it refuses. */
static int accepted(struct mtk_field *f, char *t)
{
	return !f->accept || f->accept(t, f->size, f->data);
}

int mtk_field_key(struct mtk_field *f, xkb_keysym_t ks, const char *txt, unsigned mods)
{
	char *b = f->text;
	size_t len = strlen(b), c = f->cur < len ? f->cur : len, was = c;
	int ctrl = mods & MTK_MOD_CTRL, n = (int)strlen(txt);
	int cut = f->fresh && f->selects && c > 0;      /* a selection to replace */
	char *t = malloc(f->size);
	int changed = 1;
	if (!t)
		return MTK_FIELD_NONE;
	memcpy(t, b, len + 1);
	switch (ks) {
	case XKB_KEY_Return: case XKB_KEY_KP_Enter:
		free(t);
		return MTK_FIELD_ENTER;
	case XKB_KEY_Escape:
		free(t);
		return MTK_FIELD_CANCEL;
	/* The cursor stays out of a grapheme. */
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
		c = mtk_grapheme_step(b, c, -1);
		changed = 0;
		break;
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
		c = mtk_grapheme_step(b, c, 1);
		changed = 0;
		break;
	case XKB_KEY_Home: case XKB_KEY_KP_Home:
		c = 0;
		changed = 0;
		break;
	case XKB_KEY_End: case XKB_KEY_KP_End:
		c = len;
		changed = 0;
		break;
	case XKB_KEY_BackSpace:
		if (cut) {
			memmove(t, t + c, len - c + 1);
			c = 0;
		} else if (c) {         /* the whole grapheme */
			size_t q = mtk_grapheme_step(b, c, -1);
			memmove(t + q, t + c, len - c + 1);
			c = q;
		} else {
			changed = 0;
		}
		break;
	case XKB_KEY_Delete: case XKB_KEY_KP_Delete:
		if (cut) {
			memmove(t, t + c, len - c + 1);
			c = 0;
		} else if (c < len) {
			size_t q = mtk_grapheme_step(b, c, 1);
			memmove(t + c, t + q, len - q + 1);
		} else {
			changed = 0;
		}
		break;
	default:
		if (ctrl && (ks == XKB_KEY_u || ks == XKB_KEY_U)) {
			changed = len > 0;
			t[0] = 0;
			c = 0;
			break;
		}
		if (ctrl || (mods & (MTK_MOD_ALT | MTK_MOD_LOGO)) || n <= 0 ||
		    (unsigned char)txt[0] < 0x20 || txt[0] == 0x7f) {
			free(t);
			return MTK_FIELD_NONE;
		}
		if (cut) {
			if (len - c + n >= f->size) {
				free(t);
				return MTK_FIELD_NONE;  /* would not fit: keep all */
			}
			memmove(t, t + c, len - c + 1);
			len -= c;
			c = 0;
		}
		if (len + n >= f->size) {
			free(t);
			return MTK_FIELD_NONE;
		}
		memmove(t + c + n, t + c, len - c + 1);
		memcpy(t + c, txt, n);
		c += n;
	}
	if (changed) {
		size_t before = strlen(t);
		if (!accepted(f, t)) {
			free(t);
			return MTK_FIELD_NONE;
		}
		/* The filter may have changed it: the cursor keeps its distance from
		 * the end. */
		size_t after = strlen(t);
		c = c + after >= before ? c + after - before : 0;
		memcpy(b, t, after + 1);
	}
	free(t);
	/* Deleting can join stray bytes into one character: the cursor stays on a
	 * character boundary. */
	const char *p = b;
	while ((size_t)(p - b) < c && *p)
		u8_next(&p);
	c = p - b;
	int moved = c != was || f->fresh;
	f->cur = c;
	f->fresh = 0;
	return changed ? MTK_FIELD_CHANGED : moved ? MTK_FIELD_MOVED : MTK_FIELD_NONE;
}
