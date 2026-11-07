/*
 * mfm - a minimal file manager for Wayland, in the Motif look of mwc
 * (https://github.com/reygnn/mwc). The X11 version is mfm-x11.
 *
 * libwayland, Pango and Cairo, one file, no toolkit. It reads the same
 * colours as mwc: MWC_INACTIVE / MWC_ACTIVE from the environment, else
 * Mwm*background and Mwm*activeBackground from ~/.config/mwc/mwmrc, else the
 * Motif defaults.
 *
 * Keys:   Up/Down/PgUp/PgDn/Home/End  move      Return / Right  open
 *         Backspace / Left / Alt+Up   parent    Alt+Home        home dir
 *         typing                      jump to name (type-ahead)
 *         Ctrl+L  edit path   Ctrl+H  hidden files   Ctrl+R / F5  reload
 *         Ctrl+T  terminal here   Ctrl+Q / Ctrl+W / Alt+F4  quit
 *         Esc  clear the search, else the message, else the marks
 *         Space / Insert  mark   Ctrl+A  mark all   Ctrl+C / Ctrl+X  copy /
 *         cut   Ctrl+V  paste   Delete  to the trash   Shift+Delete  delete
 *         F2  rename   F7 / Ctrl+Shift+N  new folder
 * Mouse:  click select, double-click open, wheel scroll, scrollbar drag,
 *         Ctrl+click mark, Shift+click mark a range,
 *         up-arrow button or back button (btn 8) for the parent directory.
 *         Right click in the list: a menu with the commands above; cut,
 *         copy, trash and delete take the marks if a marked entry or the
 *         empty area was clicked.
 *
 * Copy, move and permanent delete run in a child process, one item after
 * the other (cp -a, rename or mv, rm -rf --one-file-system). Nothing is
 * overwritten: a taken name gets " (copy)" or, when moving, " (2)". The
 * trash follows the freedesktop.org specification.
 *
 * Paths are kept logical, as the shell's `cd` does: after entering a
 * symlinked directory, ".." leads back to where the link is.
 *
 * Files open with $MFM_OPEN (default xdg-open); the terminal is
 * $MFM_TERMINAL, else $MWC_TERMINAL, else the first one found of foot,
 * xterm, uxterm, x-terminal-emulator, ... Both commands are split at blanks
 * (quotes group words; no other shell syntax), and the program is looked
 * up in $PATH, never in the directory being browsed.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wctype.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "version.h"

#define BEV 2           /* bevel width, as in mwm */
#define PAD 6
#define SBW 15          /* scrollbar width */
#define DCLICK_MS 400
#define TYPEAHEAD_MS 1000
#define POLL_MS 1000
#define MENU_CLICK_MS 300       /* a quicker click leaves the menu open */
#ifndef MAX            /* glib has them too */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct ent {
	char *name;
	mode_t mode;
	off_t size;
	time_t mtime;
	int dir, link, up;
	int nostat;     /* stat failed: only the name and d_type are known */
	int mark;       /* marked for copy, move or delete */
};

struct rect { int x, y, w, h; };

/* ---- state -------------------------------------------------------------- */

static struct ent *ents;
static int nents, sel, top;
static char cwd[PATH_MAX];      /* logical, absolute, never empty once up */
static struct timespec cwd_mtime;
static int cwd_unsure;        /* changed within the timestamp granularity */
static int show_hidden;
/* Ctrl+1/2/3; the same key again reverses. Folders always stay on top. */
enum { SORT_NAME, SORT_SIZE, SORT_DATE };
static int sort_by, sort_rev;
static const char *const sort_desc[3][2] = {
	{ "by name", "by name, reversed" },
	{ "by size, largest first", "by size, smallest first" },
	{ "by date, newest first", "by date, oldest first" },
};

static char status_msg[PATH_MAX + 128];
static int status_err;
/* The last error load() put in the status line, and for which directory;
 * a later complete listing of that directory removes it again. */
static char load_err[sizeof status_msg], load_err_path[PATH_MAX];
static char search[128];
static uint64_t search_ms;      /* monotonic time of the last type-ahead key */
static int editing, edit_fresh;
static char edit[PATH_MAX];
static size_t edit_cur;
/* F2 / F7: a name typed into the status line */
enum { PR_NONE, PR_RENAME, PR_MKDIR };
static int prompt, pfresh;
static char pbuf[NAME_MAX + 1], pold[NAME_MAX + 1], pdir[PATH_MAX];
static size_t pcur;
static int up_pressed, dragging, drag_off;
static uint32_t last_click;
static int last_idx = -1;
static int dirty = 1;

/* Ctrl+C / Ctrl+X: names in clip_dir to copy or move on Ctrl+V */
static char clip_dir[PATH_MAX];
static char **clip;
static int nclip, clip_move;
static unsigned clip_gen;       /* counts clipboard changes */
/* Shift+Delete waits for y/n; the names are taken when it is pressed */
static char pend_dir[PATH_MAX];
static char **pend;
static int npend;

/* The running copy, move or delete. A child process does the work and
 * writes one line per failed item to fd; EOF means it is done. */
enum { OP_COPY, OP_MOVE, OP_DELETE, OP_SEND };
static const char *const op_ing[] = { "copying", "moving", "deleting",
	"sending" };
static const char *const op_ed[] = { "copied", "moved", "deleted", "sent" };
/* Ctrl+S: MFM_SEND, looked up before the job starts; the worker inherits it. */
static char send_prog[PATH_MAX];
static struct {
	int fd, op, n, fails;
	int finished;           /* the worker said it got to its end */
	unsigned clip_gen;      /* the clipboard a move started from */
	char dir[PATH_MAX], first[NAME_MAX + 1];
	char out[512];
	size_t outlen;
} job = { .fd = -1 };

/* ---- Wayland ------------------------------------------------------------ */

/* One wl_shm buffer with a Cairo context on it. */
struct shmbuf {
	struct wl_buffer *wl;
	cairo_surface_t *cs;
	cairo_t *cr;
	void *data;
	size_t size;
	int w, h;
	int busy;       /* the compositor still reads it */
	int stale;      /* wrong size: free it as soon as it is released */
};

struct wsurf {
	struct wl_surface *wl;
	struct wl_callback *frame;      /* a frame is on its way */
	struct shmbuf *b[2];
};

/* Modifiers and held buttons, in the state of a pointer event. */
enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, BTN1_HELD = 8, BTN2_HELD = 16 };
enum { B_LEFT = 1, B_MIDDLE = 2, B_RIGHT = 3, B_WHEEL_UP = 4, B_WHEEL_DOWN = 5,
	B_BACK = 8 };

struct pev {
	int button, x, y;
	uint32_t serial, time;
	unsigned state;
};

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct wsurf main_s;
static struct xdg_surface *main_xdg;
static struct xdg_toplevel *toplevel;
static int configured;          /* the first configure has arrived */
static int need_commit;         /* a configure waits for its commit */
static int pend_w, pend_h;      /* size of the configure in progress */
static int pend_bound;          /* ... which is maximized, tiled or the like */
static int float_w = 680, float_h = 480; /* the size of its own to go back to */

static struct wl_pointer *pointer;
static struct wl_surface *ptr_surface;
static int ptr_x, ptr_y;
static unsigned btn_mask;
static double axis_acc;
static int axis_steps;
static struct wl_surface *cursor_surf;
static struct wl_cursor_image *cursor_img;

static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *kb_map;
static struct xkb_state *kb_state;
static struct xkb_compose_state *compose;
static uint32_t repeat_kc;      /* xkb keycode of the repeating key, or 0 */
static uint64_t repeat_next;
static int repeat_rate = 25, repeat_delay = 600;

static int W = 680, H = 480;
static uint32_t px_bg, px_top, px_bot, px_list, px_sel, px_trough,
	px_fg, px_folder, px_paper, px_mark;
static uint32_t xc_fg, xc_sel, xc_dim, xc_err;
static cairo_t *cr;             /* what fill, bevel and text draw on */
static PangoLayout *plo;
static int font_asc, font_desc;
static int rowh, barh;
static struct rect r_up, r_path, r_list, r_sb, r_status;

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- buffers ------------------------------------------------------------ */

static void buf_free(struct shmbuf *b)
{
	cairo_destroy(b->cr);
	cairo_surface_destroy(b->cs);
	wl_buffer_destroy(b->wl);
	munmap(b->data, b->size);
	free(b);
}

static void buf_release(void *data, struct wl_buffer *wl)
{
	struct shmbuf *b = data;
	b->busy = 0;
	if (b->stale)
		buf_free(b);
}

static const struct wl_buffer_listener buf_listener = { buf_release };

static struct shmbuf *buf_new(int w, int h)
{
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
	size_t size = (size_t)stride * h;
	struct shmbuf *b = calloc(1, sizeof *b);
	int fd = memfd_create("mfm", MFD_CLOEXEC);
	void *data = MAP_FAILED;
	if (b && fd >= 0 && ftruncate(fd, size) == 0)
		data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mfm: no memory for a %dx%d buffer: %s\n", w, h,
			strerror(errno));
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	b->data = data;
	b->size = size;
	b->w = w;
	b->h = h;
	b->cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, w, h,
		stride);
	b->cr = cairo_create(b->cs);
	cairo_set_antialias(b->cr, CAIRO_ANTIALIAS_NONE);       /* not the text */
	return b;
}

/* A buffer of this size to draw on, or NULL while the compositor holds both;
 * its release wakes the main loop. */
static struct shmbuf *wsurf_buffer(struct wsurf *s, int w, int h)
{
	for (int i = 0; i < 2; i++) {
		struct shmbuf *b = s->b[i];
		if (!b || (b->w == w && b->h == h))
			continue;
		if (b->busy)
			b->stale = 1;
		else
			buf_free(b);
		s->b[i] = NULL;
	}
	for (int i = 0; i < 2; i++)
		if (s->b[i] && !s->b[i]->busy)
			return s->b[i];
	for (int i = 0; i < 2; i++)
		if (!s->b[i])
			return s->b[i] = buf_new(w, h);
	return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct wsurf *s = data;
	wl_callback_destroy(cb);
	s->frame = NULL;
}

static const struct wl_callback_listener frame_listener = { frame_done };

static void wsurf_present(struct wsurf *s, struct shmbuf *b)
{
	cairo_surface_flush(b->cs);
	if (s->frame)
		wl_callback_destroy(s->frame);
	s->frame = wl_surface_frame(s->wl);
	wl_callback_add_listener(s->frame, &frame_listener, s);
	wl_surface_attach(s->wl, b->wl, 0, 0);
	wl_surface_damage_buffer(s->wl, 0, 0, b->w, b->h);
	wl_surface_commit(s->wl);
	b->busy = 1;
}

static void wsurf_destroy(struct wsurf *s)
{
	if (s->frame)
		wl_callback_destroy(s->frame);
	wl_surface_destroy(s->wl);
	for (int i = 0; i < 2; i++)
		if (s->b[i])
			buf_free(s->b[i]);
	memset(s, 0, sizeof *s);
}

/* ---- UTF-8 -------------------------------------------------------------- */

/* Length of the UTF-8 sequence at s (1 for ASCII), or 0 if it is invalid. */
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

/* Next code point; a byte that is not valid UTF-8 counts as Latin-1, as
 * file names from old disks and network shares often are. */
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

/* Start of the last character in s[0..len). */
static size_t u8_prev(const char *s, size_t len)
{
	const char *p = s, *last = s;
	while ((size_t)(p - s) < len) {
		last = p;
		u8_next(&p);
	}
	return last - s;
}

/* Characters that only reorder what is drawn: a name with U+202E shows
 * "fdp.exe" as "exe.pdf". */
static int is_bidi_control(uint32_t c)
{
	return c == 0x61c || c == 0x200e || c == 0x200f ||
		(c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069);
}

/* A valid UTF-8 copy of s for drawing (static buffer): Latin-1 bytes are
 * converted, C1 control bytes and bidi controls become U+FFFD. */
static const char *disp(const char *s)
{
	static char out[3 * PATH_MAX + 8];
	size_t o = 0;
	while (*s && o + 4 < sizeof out) {
		uint32_t cp;
		int n = u8_len((const unsigned char *)s, &cp);
		if (n && is_bidi_control(cp)) {
			memcpy(out + o, "\xef\xbf\xbd", 3);
			o += 3;
			s += n;
			continue;
		}
		if (n) {
			memcpy(out + o, s, n);
			o += n;
			s += n;
			continue;
		}
		unsigned char c = *s++;
		if (c < 0xa0) {
			memcpy(out + o, "\xef\xbf\xbd", 3);
			o += 3;
		} else {
			out[o++] = 0xc0 | c >> 6;
			out[o++] = 0x80 | (c & 0x3f);
		}
	}
	out[o] = 0;
	return out;
}

static int is_combining(uint32_t c)
{
	return (c >= 0x300 && c <= 0x36f) || (c >= 0x1ab0 && c <= 0x1aff) ||
		(c >= 0x1dc0 && c <= 0x1dff) || (c >= 0x20d0 && c <= 0x20ff) ||
		(c >= 0xfe20 && c <= 0xfe2f);
}

/* Lower case for type-ahead. towlower() only knows non-ASCII letters in a
 * UTF-8 locale, so the common European ranges are folded by hand too. */
static uint32_t fold(uint32_t c)
{
	if (c < 0x80)
		return c >= 'A' && c <= 'Z' ? c + 32 : c;
	wint_t l = towlower((wint_t)c);
	if (l != (wint_t)c)
		return l;
	if ((c >= 0xc0 && c <= 0xde && c != 0xd7) ||
	    (c >= 0x391 && c <= 0x3a9) || (c >= 0x410 && c <= 0x42f))
		return c + 0x20;
	if (c >= 0x400 && c <= 0x40f)
		return c + 0x50;
	if (((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177)) &&
	    !(c & 1))
		return c + 1;
	if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) &&
	    (c & 1))
		return c + 1;
	return c;
}

static int prefix_ci(const char *name, const char *pre)
{
	while (*pre) {
		if (!*name || fold(u8_next(&name)) != fold(u8_next(&pre)))
			return 0;
	}
	return 1;
}

/* ---- colours (same rules as mwm's draw.c) ------------------------------- */

static uint32_t mix(uint32_t rgb, uint32_t to, float f)
{
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float c = (rgb >> sh) & 0xff, t = (to >> sh) & 0xff;
		out |= (uint32_t)(c + (t - c) * f + 0.5f) << sh;
	}
	return out;
}

static int is_light(uint32_t rgb)
{
	unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	return 299 * r + 587 * g + 114 * b > 140000;
}

static uint32_t contrast(uint32_t bg)
{
	return is_light(bg) ? 0x000000 : 0xffffff;
}

/* What mwc reads: "#rrggbb", names like "gold", and rgb:r/g/b with 1 to 4 hex
 * digits per channel. */
static int parse_color(const char *s, uint32_t *out)
{
	PangoColor c;
	if (!s || !*s)
		return 0;
	if (!strncasecmp(s, "rgb:", 4)) {
		uint32_t rgb = 0;
		s += 4;
		for (int i = 0; i < 3; i++) {
			char *end;
			unsigned long v = strtoul(s, &end, 16);
			int digits = end - s;
			if (digits < 1 || digits > 4 || *end != (i < 2 ? '/' : '\0'))
				return 0;
			unsigned long max = (1ul << (4 * digits)) - 1;
			rgb = rgb << 8 | (v * 255 + max / 2) / max;
			s = end + 1;
		}
		*out = rgb;
		return 1;
	}
	if (!pango_color_parse(&c, s))
		return 0;
	*out = (uint32_t)(c.red >> 8) << 16 | (c.green >> 8) << 8 | (c.blue >> 8);
	return 1;
}

/* Last global "Mwm*key: value" line of ~/.config/mwc/mwmrc. */
static char *mwmrc_get(const char *key, char *val, size_t n)
{
	char path[PATH_MAX], line[512];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	int len;
	if (xdg && *xdg)
		len = snprintf(path, sizeof path, "%s/mwc/mwmrc", xdg);
	else
		len = snprintf(path, sizeof path, "%s/.config/mwc/mwmrc",
			home ? home : "");
	if (len < 0 || (size_t)len >= sizeof path)
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *found = NULL;
	size_t kl = strlen(key);
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (isspace((unsigned char)*p))
			p++;
		/* As mwc reads it: any case, "." or "*", blanks before the ":". */
		char *colon = strchr(p, ':');
		if (!colon)
			continue;
		char *ke = colon;
		while (ke > p && isspace((unsigned char)ke[-1]))
			ke--;
		if (strncasecmp(p, "mwm", 3) || (p[3] != '*' && p[3] != '.') ||
		    (size_t)(ke - (p + 4)) != kl || strncasecmp(p + 4, key, kl))
			continue;
		p = colon + 1;
		while (isspace((unsigned char)*p))
			p++;
		char *e = p + strlen(p);
		while (e > p && isspace((unsigned char)e[-1]))
			*--e = 0;
		if ((size_t)snprintf(val, n, "%s", p) < n)
			found = val;
	}
	fclose(f);
	return found;
}

static uint32_t setting(const char *env, const char *res, uint32_t def)
{
	uint32_t rgb;
	char v[256];
	if (parse_color(getenv(env), &rgb))
		return rgb;
	if (parse_color(mwmrc_get(res, v, sizeof v), &rgb))
		return rgb;
	return def;
}

static void theme_init(void)
{
	uint32_t bg = setting("MWC_INACTIVE", "background", 0xaeb2c3);
	uint32_t act = setting("MWC_ACTIVE", "activeBackground", 0x7387ad);
	uint32_t fg = contrast(bg);

	px_bg = bg;
	px_top = mix(bg, 0xffffff, 0.55f);
	px_bot = mix(bg, 0x000000, 0.5f);
	px_list = mix(bg, 0xffffff, 0.3f);
	px_trough = mix(bg, 0x000000, 0.12f);
	px_sel = act;
	px_mark = mix(mix(bg, 0xffffff, 0.3f), act, 0.4f);
	px_fg = fg;
	px_folder = 0xc9a85c;
	px_paper = 0xf6f6f6;
	xc_fg = fg;
	xc_sel = contrast(act);
	xc_dim = mix(bg, fg, 0.6f);
	xc_err = 0x8a0303;

	/* fontconfig patterns (Sans:bold:size=10, as mfm-x11 took them) mean
	 * nothing to Pango, which would quietly pick some other font. */
	const char *f = getenv("MFM_FONT");
	if (f && strchr(f, ':')) {
		fprintf(stderr, "mfm: ignoring MFM_FONT \"%s\": it takes Pango "
			"names like \"Sans Bold 10\"\n", f);
		f = NULL;
	}
	PangoFontDescription *desc =
		pango_font_description_from_string(f && *f ? f : "Sans 10");
	/* "Monospace" has no size, and metrics without one are all zero. */
	if (pango_font_description_get_size(desc) <= 0)
		pango_font_description_set_size(desc, 10 * PANGO_SCALE);
	PangoContext *ctx = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	/* Left to right as a whole, or a Hebrew name shows its extension in
	 * front of it. */
	pango_context_set_base_dir(ctx, PANGO_DIRECTION_LTR);
	plo = pango_layout_new(ctx);
	pango_layout_set_auto_dir(plo, FALSE);
	pango_layout_set_font_description(plo, desc);
	/* A newline in a file name must not make a second line. */
	pango_layout_set_single_paragraph_mode(plo, TRUE);
	PangoFontMetrics *m = pango_context_get_metrics(ctx, desc, NULL);
	font_asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(m));
	font_desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(m));
	pango_font_metrics_unref(m);
	pango_font_description_free(desc);
	rowh = font_asc + font_desc + 4;
	barh = rowh + 6;
}

/* ---- small helpers ------------------------------------------------------ */

static void set_status(int err, const char *fmt, ...)
{
	va_list ap;
	int saved = errno;
	va_start(ap, fmt);
	vsnprintf(status_msg, sizeof status_msg, fmt, ap);
	va_end(ap);
	status_err = err;
	dirty = 1;
	errno = saved;
}

static void clear_status(void)
{
	if (status_msg[0]) {
		status_msg[0] = 0;
		dirty = 1;
	}
}

static void note_load_error(const char *path)
{
	snprintf(load_err, sizeof load_err, "%s", status_msg);
	snprintf(load_err_path, sizeof load_err_path, "%s", path);
}

static int in(struct rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* dir + "/" + name; -1 with errno ENAMETOOLONG instead of truncating. */
static int join(char *out, const char *dir, const char *name)
{
	int n = snprintf(out, PATH_MAX, "%s/%s", strcmp(dir, "/") ? dir : "",
		name);
	if (n < 0 || n >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

/* Cut the last component off an absolute path, in place. */
static void parent_of(char *p)
{
	char *s = strrchr(p, '/');
	if (!s || s == p)
		strcpy(p, "/");
	else
		*s = 0;
}

/* Collapse "//", drop "." and resolve ".." textually, like `cd -L`.
 * `in` must be absolute. -1 if the result does not fit in PATH_MAX. */
static int normalize(const char *in, char *out)
{
	size_t o = 1;
	out[0] = '/';
	for (const char *p = in; *p;) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *e = strchr(p, '/');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n == 1 && p[0] == '.') {
			/* nothing */
		} else if (n == 2 && p[0] == '.' && p[1] == '.') {
			while (o > 1 && out[o - 1] != '/')
				o--;
			if (o > 1)
				o--;
		} else {
			if (o > 1)
				out[o++] = '/';
			if (o + n >= PATH_MAX)
				return -1;
			memcpy(out + o, p, n);
			o += n;
		}
		p += n;
	}
	out[o] = 0;
	return 0;
}

static void human(off_t s, char *o, size_t n)
{
	const char *u = "BKMGTP";
	double v = s;
	int i = 0;
	while (v >= 1024 && i < 5) {
		v /= 1024;
		i++;
	}
	if (i == 0)
		snprintf(o, n, "%lld B", (long long)s);
	else
		snprintf(o, n, "%.1f %c", v, u[i]);
}

static void perms(const struct ent *e, char o[11])
{
	const char *r = "rwxrwxrwx";
	o[0] = e->link ? 'l' : e->dir ? 'd' : '-';
	for (int i = 0; i < 9; i++)
		o[1 + i] = e->nostat ? '?' :
			(e->mode & (0400 >> i)) ? r[i] : '-';
	o[10] = 0;
}

/* ---- directory ---------------------------------------------------------- */

static int ent_cmp(const void *a, const void *b)
{
	const struct ent *x = a, *y = b;
	if (x->up != y->up)
		return y->up - x->up;
	if (x->dir != y->dir)
		return y->dir - x->dir;
	int r = 0;
	/* Unknown sizes and dates go last; folder sizes say nothing, so
	 * folders stay by name when sorting by size. Largest and newest
	 * come first. */
	if (sort_by != SORT_NAME && x->nostat != y->nostat)
		return x->nostat - y->nostat;
	if (sort_by == SORT_SIZE && !x->dir && x->size != y->size)
		r = x->size > y->size ? -1 : 1;
	else if (sort_by == SORT_DATE && x->mtime != y->mtime)
		r = x->mtime > y->mtime ? -1 : 1;
	if (!r)
		r = strcoll(x->name, y->name);
	if (!r)
		r = strcmp(x->name, y->name);
	return sort_rev ? -r : r;
}

static void free_ents(struct ent *v, int n)
{
	for (int i = 0; i < n; i++)
		free(v[i].name);
	free(v);
}

/* A Wayland message must fit in 4096 bytes and a path need not, so a very
 * long one keeps its end. */
static void set_title(void)
{
	enum { MAX_TITLE = 1024 };
	static char t[sizeof "mfm: " + MAX_TITLE];
	const char *d = disp(cwd);
	size_t n = strlen(d);
	if (n > MAX_TITLE) {
		d += n - MAX_TITLE;
		while ((*d & 0xc0) == 0x80)
			d++;
	}
	snprintf(t, sizeof t, "mfm: %s", d);
	xdg_toplevel_set_title(toplevel, t);
}

static int rows_vis(void)
{
	return MAX(1, (r_list.h - 2 * BEV - 4) / rowh);
}

static int sel_visible(void)
{
	return sel >= top && sel < top + rows_vis();
}

static void clamp_top(void)
{
	top = MAX(0, MIN(top, nents - rows_vis()));
}

static void show_sel(void)
{
	int vis = rows_vis();
	sel = MAX(0, MIN(sel, nents - 1));
	if (sel < top)
		top = sel;
	if (sel >= top + vis)
		top = sel - vis + 1;
	clamp_top();
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Carry the marks of the current listing over to v, by name. */
static void keep_marks(struct ent *v, int n)
{
	int k = 0;
	for (int i = 0; i < nents; i++)
		k += ents[i].mark;
	if (!k)
		return;
	char **m = malloc(k * sizeof *m);
	if (!m)
		return;         /* the marks are lost, nothing worse */
	k = 0;
	for (int i = 0; i < nents; i++)
		if (ents[i].mark)
			m[k++] = ents[i].name;
	qsort(m, k, sizeof *m, name_cmp);
	for (int i = 0; i < n; i++)
		v[i].mark = !v[i].up &&
			bsearch(&v[i].name, m, k, sizeof *m, name_cmp);
	free(m);
}

/*
 * Read `path` (absolute, normalized). Selects `select` if present, else
 * the entry at index `fallback`. Keeps the old listing on failure and
 * returns -1 with errno set.
 */
static int load(const char *path, const char *select, int fallback)
{
	DIR *d = opendir(path);
	if (!d) {
		set_status(1, "%s: %s", path, strerror(errno));
		note_load_error(path);
		return -1;
	}
	int fd = dirfd(d), n = 0, cap = 64, rerr = 0;
	struct stat ds;
	/* Taken before reading, so a change made while we read is newer
	 * than what we record and the next poll picks it up. */
	struct timespec mt = { 0, 0 };
	if (fstat(fd, &ds) == 0)
		mt = ds.st_mtim;
	/* A file system with coarse timestamps can put a later change into
	 * the same tick; keep re-reading until the directory has been quiet
	 * for a moment. */
	time_t now = time(NULL);
	int unsure = mt.tv_sec >= now - 1 && mt.tv_sec <= now + 1;
	struct ent *v = calloc(cap, sizeof *v);
	if (!v)
		goto oom;
	if (strcmp(path, "/")) {
		if (!(v[0].name = strdup("..")))
			goto oom;
		v[0].dir = v[0].up = 1;
		v[0].mode = 0755;
		n = 1;
	}
	struct dirent *de;
	for (errno = 0; (de = readdir(d)); errno = 0) {
		const char *nm = de->d_name;
		if (!strcmp(nm, ".") || !strcmp(nm, ".."))
			continue;
		if (!show_hidden && nm[0] == '.')
			continue;
		if (n == cap) {
			struct ent *nv = realloc(v, 2 * cap * sizeof *v);
			if (!nv)
				goto oom;
			v = nv;
			cap *= 2;
		}
		struct ent *e = &v[n];
		struct stat st, lst;
		memset(e, 0, sizeof *e);
		if (!(e->name = strdup(nm)))
			goto oom;
		n++;
		if (fstatat(fd, nm, &lst, AT_SYMLINK_NOFOLLOW) == 0) {
			e->link = S_ISLNK(lst.st_mode);
			if (!e->link || fstatat(fd, nm, &st, 0) != 0)
				st = lst;
			e->mode = st.st_mode;
			e->size = st.st_size;
			e->mtime = st.st_mtime;
			e->dir = S_ISDIR(st.st_mode);
		} else {
			/* e.g. a directory with read but no search permission */
			e->nostat = 1;
			e->dir = de->d_type == DT_DIR;
			e->link = de->d_type == DT_LNK;
		}
	}
	rerr = errno;
	closedir(d);
	qsort(v, n, sizeof *v, ent_cmp);

	cwd_mtime = mt;
	cwd_unsure = unsure;
	if (!strcmp(path, cwd))
		keep_marks(v, n);
	free_ents(ents, nents);
	ents = v;
	nents = n;
	if (path != cwd && strcmp(path, cwd)) {
		snprintf(cwd, sizeof cwd, "%s", path);
		last_idx = -1;          /* no double click across listings */
	}

	sel = MAX(0, MIN(fallback, n - 1));
	for (int i = 0; select && i < n; i++)
		if (!strcmp(v[i].name, select)) {
			sel = i;
			break;
		}
	top = 0;
	show_sel();
	if (toplevel)
		set_title();
	if (rerr) {
		set_status(1, "%s: listing incomplete: %s", path,
			strerror(rerr));
		note_load_error(cwd);
	} else if (load_err[0] && !strcmp(load_err_path, cwd)) {
		if (!strcmp(status_msg, load_err))
			clear_status();         /* that error no longer holds */
		load_err[0] = 0;
	}
	dirty = 1;
	return 0;
oom:
	closedir(d);
	if (v)
		free_ents(v, n);
	set_status(1, "%s: %s", path, strerror(ENOMEM));
	note_load_error(path);
	errno = ENOMEM;
	return -1;
}

/* Re-read cwd, keeping the selection (or its position) and the scroll
 * position; the selection is only scrolled to if it was in view. */
static int reload(void)
{
	char keep[NAME_MAX + 1] = "";
	int t = top, old = sel, was = sel_visible();
	int clicked = last_idx >= 0 && last_idx == sel;
	if (sel >= 0 && sel < nents)
		snprintf(keep, sizeof keep, "%s", ents[sel].name);
	if (load(cwd, keep, old) < 0)
		return -1;
	/* A first click on an entry that is still there stays a first click,
	 * wherever the entry moved to. */
	last_idx = clicked && sel < nents && !strcmp(ents[sel].name, keep) ?
		sel : -1;
	top = t;
	if (was)
		show_sel();
	else
		clamp_top();
	return 0;
}

/* ---- spawning ----------------------------------------------------------- */

/*
 * Look prog up like execvp, but skip empty and relative $PATH entries:
 * children start in the browsed directory, and a program lying there must
 * not shadow the real one. Returns 0 or an errno value.
 */
static int find_prog(const char *prog, char *out)
{
	if (strchr(prog, '/')) {
		if (!realpath(prog, out))
			return errno;
		return access(out, X_OK) == 0 ? 0 : errno;
	}
	const char *path = getenv("PATH");
	if (!path || !*path)
		path = "/usr/local/bin:/usr/bin:/bin";
	int err = ENOENT;
	for (const char *p = path;;) {
		const char *e = strchr(p, ':');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		struct stat st;
		if (n > 0 && p[0] == '/' &&
		    n + 1 + strlen(prog) < PATH_MAX) {
			memcpy(out, p, n);
			out[n] = '/';
			strcpy(out + n + 1, prog);
			if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
				if (access(out, X_OK) == 0)
					return 0;
				err = EACCES;
			}
		}
		if (!e)
			break;
		p = e + 1;
	}
	return err;
}

/* Split "xterm -fa 'DejaVu Sans Mono'" into words; quotes group, nothing
 * else is special. Returns the word count, or -1 if malformed or too long. */
static int split_cmd(const char *cmd, char *b, size_t bsz, char **argv,
	int max)
{
	int argc = 0;
	size_t o = 0;
	const char *p = cmd;
	for (;;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (argc >= max - 1 || o >= bsz)
			return -1;
		argv[argc++] = b + o;
		char q = 0;
		for (; *p && (q || (*p != ' ' && *p != '\t')); p++) {
			if (!q && (*p == '"' || *p == '\'')) {
				q = *p;
			} else if (q && *p == q) {
				q = 0;
			} else {
				if (o + 1 >= bsz)
					return -1;
				b[o++] = *p;
			}
		}
		if (q)
			return -1;
		b[o++] = 0;
	}
	argv[argc] = NULL;
	return argc;
}

/* fork + exec; returns the exec errno, or 0 once the program runs. */
static int spawn(const char *prog, char *const argv[])
{
	/* Start in the browsed directory if we may, else somewhere sane. */
	const char *dir = cwd;
	if (access(cwd, X_OK) != 0) {
		dir = getenv("HOME");
		if (!dir || access(dir, X_OK) != 0)
			dir = "/";
	}
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0)
		return errno;
	pid_t pid = fork();
	if (pid < 0) {
		int e = errno;
		close(p[0]);
		close(p[1]);
		return e;
	}
	if (pid == 0) {
		close(p[0]);
		setsid();
		signal(SIGCHLD, SIG_DFL);
		if (chdir(dir) < 0) {
			dir = "/";
			if (chdir(dir) < 0) {
				/* run anyway */
			}
		}
		/* The logical path, so a shell shows the symlinked name. */
		setenv("PWD", dir, 1);
		execv(prog, argv);
		int e = errno;
		if (write(p[1], &e, sizeof e) < 0) {
			/* nothing left to tell */
		}
		_exit(127);
	}
	close(p[1]);
	int e = 0;
	ssize_t r;
	while ((r = read(p[0], &e, sizeof e)) < 0 && errno == EINTR)
		;
	close(p[0]);
	return r == (ssize_t)sizeof e ? e : 0;
}

/* Run a user command with an optional extra argument. 0, an errno value,
 * or -1 if the command line cannot be parsed. The program's name goes to
 * `name` for messages. */
static int run(const char *cmd, const char *arg, char *name, size_t nsz)
{
	char b[2048], prog[PATH_MAX], *argv[34];
	int argc = split_cmd(cmd, b, sizeof b, argv, 33);
	snprintf(name, nsz, "%s", argc > 0 ? argv[0] : "");
	if (argc <= 0)
		return -1;
	if (arg) {
		argv[argc++] = (char *)arg;
		argv[argc] = NULL;
	}
	int e = find_prog(argv[0], prog);
	return e ? e : spawn(prog, argv);
}

static void open_file(const char *path)
{
	const char *op = getenv("MFM_OPEN");
	char name[256];
	int custom = op && *op;
	if (!custom)
		op = "xdg-open";
	int e = run(op, path, name, sizeof name);
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	if (e < 0)
		set_status(1, "MFM_OPEN: cannot parse the command");
	else if (e)
		set_status(1, "cannot run %s: %s%s", name, strerror(e),
			e == ENOENT && !custom ?
			" (install xdg-utils or set MFM_OPEN)" : "");
	else
		set_status(0, "opened %s", base);
}

static void open_terminal(void)
{
	static const char *const terms[] = { "foot", "xterm", "uxterm",
		"x-terminal-emulator", "urxvt", "alacritty", "konsole",
		"kitty", "xfce4-terminal", "gnome-terminal" };
	const char *var = "MFM_TERMINAL", *t = getenv(var);
	char p[PATH_MAX], name[256];
	if (!t || !*t)
		t = getenv(var = "MWC_TERMINAL");
	if (!t || !*t) {
		t = terms[0];
		for (size_t i = 0; i < sizeof terms / sizeof *terms; i++)
			if (find_prog(terms[i], p) == 0) {
				t = terms[i];
				break;
			}
	}
	int e = run(t, NULL, name, sizeof name);
	if (e < 0)
		set_status(1, "%s: cannot parse the command", var);
	else if (e)
		set_status(1, "cannot run %s: %s (set MFM_TERMINAL)", name,
			strerror(e));
}

/* ---- marks -------------------------------------------------------------- */

static int nmarked(void)
{
	int k = 0;
	for (int i = 0; i < nents; i++)
		k += ents[i].mark;
	return k;
}

static void toggle_mark(int i)
{
	if (i >= 0 && i < nents && !ents[i].up) {
		ents[i].mark = !ents[i].mark;
		dirty = 1;
	}
}

/* Ctrl+A: mark everything, or unmark everything if all is marked. */
static void mark_all(void)
{
	int all = 1;
	for (int i = 0; i < nents; i++)
		if (!ents[i].up && !ents[i].mark)
			all = 0;
	for (int i = 0; i < nents; i++)
		ents[i].mark = !all && !ents[i].up;
	dirty = 1;
}

static void unmark_all(void)
{
	for (int i = 0; i < nents; i++)
		ents[i].mark = 0;
	dirty = 1;
}

static void free_names(char **v, int n)
{
	for (int i = 0; v && i < n; i++)
		free(v[i]);
	free(v);
}

/* The names an operation works on: the marked entries, else the selected
 * one. NULL (with a message) if there are none. */
static char **targets(int *n)
{
	int k = nmarked(), use_sel = !k;
	*n = 0;
	if (use_sel) {
		if (sel < 0 || sel >= nents || ents[sel].up) {
			set_status(1, "nothing selected");
			return NULL;
		}
		k = 1;
	}
	char **v = calloc(k, sizeof *v);
	int j = 0;
	for (int i = 0; v && i < nents && j < k; i++) {
		if (use_sel ? i != sel : !ents[i].mark)
			continue;
		if (!(v[j++] = strdup(ents[i].name))) {
			free_names(v, k);
			v = NULL;
		}
	}
	if (!v) {
		set_status(1, "%s", strerror(ENOMEM));
		return NULL;
	}
	*n = k;
	return v;
}

/* ---- safe renames ------------------------------------------------------- */

/* The longest prefix of s[0..n) of at most max bytes that does not end
 * inside a UTF-8 sequence. */
static int u8_fit(const char *s, int n, int max)
{
	if (n <= max)
		return n;
	while (max > 0 && (s[max] & 0xc0) == 0x80)
		max--;
	return max;
}

/* Which mount `path` is on: the mount ID where the kernel reports one
 * (it tells bind mounts and btrfs subvolumes apart), else the device. */
static int mount_id(const char *path, int follow, uint64_t *id)
{
	struct statx sx;
	if (statx(AT_FDCWD, path, follow ? 0 : AT_SYMLINK_NOFOLLOW,
	    STATX_MNT_ID, &sx) < 0)
		return -1;
	if (sx.stx_mask & STATX_MNT_ID)
		*id = sx.stx_mnt_id;
	else
		*id = 1ULL << 63 | (uint64_t)sx.stx_dev_major << 32 |
			sx.stx_dev_minor;
	return 0;
}

/* Is path (not followed) a mount point, i.e. on another mount than the
 * directory it is in? */
static int is_mount(const char *path)
{
	char par[PATH_MAX];
	uint64_t a, b;
	snprintf(par, sizeof par, "%s", path);
	parent_of(par);
	return mount_id(path, 0, &a) == 0 && mount_id(par, 1, &b) == 0 &&
		a != b;
}

/* Is anything mounted below the directory path? -1 if that cannot be told.
 * rm --one-file-system only compares devices, so it walks into a bind mount of
 * the same file system and empties what that shows. */
static int mount_below(const char *path)
{
	char real[PATH_MAX];
	char *line = NULL;
	size_t cap = 0;
	int found = 0;
	if (!realpath(path, real))
		return -1;
	FILE *f = fopen("/proc/self/mountinfo", "r");
	if (!f)
		return -1;
	size_t n = strlen(real);
	while (!found && getline(&line, &cap, f) > 0) {
		/* The mount point is the fifth field, with blanks and the like
		 * as \ooo. */
		char *m = line;
		for (int i = 0; i < 4 && m; i++)
			if ((m = strchr(m, ' ')))
				m++;
		if (!m)
			continue;
		char *o = m;
		for (const char *r = m; *r && *r != ' ' && *r != '\n'; r++) {
			if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] &&
			    r[3]) {
				*o++ = (r[1] - '0') << 6 | (r[2] - '0') << 3 |
					(r[3] - '0');
				r += 3;
			} else {
				*o++ = *r;
			}
		}
		*o = 0;
		found = !strncmp(m, real, n) && m[n] == '/';
	}
	free(line);
	fclose(f);
	return found;
}

static int same_dir(const char *a, const char *b)
{
	struct stat x, y;
	return !strcmp(a, b) || (stat(a, &x) == 0 && stat(b, &y) == 0 &&
		x.st_dev == y.st_dev && x.st_ino == y.st_ino);
}

/* Is directory dir the same as p, or inside it (physically)? */
static int inside(const char *dir, const char *p)
{
	char a[PATH_MAX], b[PATH_MAX];
	if (!realpath(dir, a) || !realpath(p, b))
		return 0;
	size_t n = strlen(b);
	return !strncmp(a, b, n) && (a[n] == 0 || a[n] == '/' || n == 1);
}

/* rename() that never replaces an existing `to`. */
static int rename_new(const char *from, const char *to)
{
	struct stat st;
	if (renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0)
		return 0;
	if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP)
		return -1;
	/* No atomic no-replace on this file system (NFS, some FUSE). A
	 * hard link never replaces anything; directories can't have one. */
	if (lstat(from, &st) < 0)
		return -1;
	if (!S_ISDIR(st.st_mode)) {
		if (link(from, to) == 0) {
			if (unlink(from) == 0)
				return 0;
			int e = errno;
			unlink(to);
			errno = e;
			return -1;
		}
		if (errno == EEXIST)
			return -1;
	}
	if (lstat(to, &st) == 0) {
		errno = EEXIST;
		return -1;
	}
	return rename(from, to);
}

/* A name for `name` in dir that is not taken: the name itself, else
 * "name (copy).ext", "name (copy 2).ext", ... for a copy and "name (2).ext",
 * ... for a move; directories keep dots in the name. A name that gets too
 * long is shortened before the suffix. */
static int free_name(const char *dir, const char *name, int isdir, int move,
	char *out)
{
	const char *dot = isdir ? NULL : strrchr(name, '.');
	int len = strlen(name);
	int stem = dot && dot != name && len - (dot - name) <= 32 ?
		(int)(dot - name) : len;
	char p[PATH_MAX], sfx[32];
	struct stat st;
	for (int k = 0; k < 10000; k++) {
		if (k == 0)
			sfx[0] = 0;
		else if (move)
			snprintf(sfx, sizeof sfx, " (%d)", k + 1);
		else if (k == 1)
			snprintf(sfx, sizeof sfx, " (copy)");
		else
			snprintf(sfx, sizeof sfx, " (copy %d)", k);
		int room = NAME_MAX - (int)strlen(sfx) - (len - stem);
		int s = u8_fit(name, stem, room);
		snprintf(out, NAME_MAX + 1, "%.*s%s%s", s, name, sfx,
			name + stem);
		if (join(p, dir, out) < 0)
			return -1;
		if (lstat(p, &st) == 0)
			continue;
		if (errno != ENOENT)
			return -1;
		return 0;
	}
	errno = EEXIST;
	return -1;
}

/* Rename `from` into dir under a free name for `name`. The name is picked
 * right before the rename, which refuses to replace anything, and picked
 * again if something took it in between. The final name goes to out. */
static int place(const char *from, const char *dir, const char *name,
	int isdir, int move, char *out)
{
	char to[PATH_MAX];
	for (int tries = 0; tries < 100; tries++) {
		if (free_name(dir, name, isdir, move, out) < 0 ||
		    join(to, dir, out) < 0)
			return -1;
		if (rename_new(from, to) == 0)
			return 0;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}

/* place() for what may be a directory without write permission for its owner:
 * cp -a keeps that, and renaming a directory to another parent has to update its
 * "..". It gets its mode back in either case. */
static int place_any(const char *from, const char *dir, const char *name,
	int isdir, int move, char *out)
{
	struct stat st;
	int unlock = isdir && lstat(from, &st) == 0 && S_ISDIR(st.st_mode) &&
		!(st.st_mode & S_IWUSR);
	if (unlock && chmod(from, (st.st_mode & 07777) | S_IWUSR) < 0)
		unlock = 0;
	int r = place(from, dir, name, isdir, move, out), e = errno;
	if (unlock) {
		char to[PATH_MAX];
		const char *now = from;
		if (r == 0 && join(to, dir, out) == 0)
			now = to;
		chmod(now, st.st_mode & 07777);
	}
	errno = e;
	return r;
}

/* ---- trash (freedesktop.org Trash specification) ------------------------ */

/* mkdir -p with mode 0700 for what is created. */
static int mkdirs(const char *path)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s", path);
	for (char *s = p + 1;; s++) {
		if (*s && *s != '/')
			continue;
		char c = *s;
		*s = 0;
		if (mkdir(p, 0700) < 0 && errno != EEXIST)
			return -1;
		*s = c;
		if (!c)
			return 0;
	}
}

/* $XDG_DATA_HOME/Trash, by default ~/.local/share/Trash. */
static int home_trash(char *out, int create)
{
	const char *d = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	int n;
	if (d && d[0] == '/')
		n = snprintf(out, PATH_MAX, "%s/Trash", d);
	else if (h && *h)
		n = snprintf(out, PATH_MAX, "%s/.local/share/Trash", h);
	else
		n = -1;
	if (n < 0 || n >= PATH_MAX - 32) {
		errno = n < 0 ? ENOENT : ENAMETOOLONG;
		return -1;
	}
	return create ? mkdirs(out) : 0;
}

/* Is the physical directory rp a trash or inside one? */
static int in_trash(const char *rp)
{
	char t[PATH_MAX], rt[PATH_MAX], own[32];
	if (home_trash(t, 0) == 0 && realpath(t, rt) && inside(rp, rt))
		return 1;
	snprintf(own, sizeof own, ".Trash-%u", (unsigned)getuid());
	for (const char *s = rp; (s = strchr(s, '/'));) {
		s++;
		size_t n = strcspn(s, "/");
		if ((n == 6 && !strncmp(s, ".Trash", 6)) ||
		    (n == strlen(own) && !strncmp(s, own, n)))
			return 1;
	}
	return 0;
}

/* A directory we may use as a trash: ours, not a link. */
static int own_dir(const char *p)
{
	struct stat st;
	if (mkdir(p, 0700) < 0 && errno != EEXIST)
		return -1;
	if (lstat(p, &st) < 0)
		return -1;
	if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

/* The trash for files on mount `mnt` whose physical parent is rpar: the
 * top directory of that mount goes to top, the trash to out. That is
 * $topdir/.Trash/$uid if the administrator set up a shared .Trash (a real
 * directory with the sticky bit), else $topdir/.Trash-$uid. */
static int topdir_trash(const char *rpar, uint64_t mnt, char *top,
	char *out)
{
	char up[PATH_MAX];
	struct stat st;
	uint64_t id;
	snprintf(top, PATH_MAX, "%s", rpar);
	while (strcmp(top, "/")) {
		snprintf(up, sizeof up, "%s", top);
		parent_of(up);
		if (mount_id(up, 1, &id) < 0 || id != mnt)
			break;
		snprintf(top, PATH_MAX, "%s", up);
	}
	const char *tp = strcmp(top, "/") ? top : "";
	unsigned uid = getuid();
	if (strlen(tp) >= PATH_MAX - 32) {
		errno = ENAMETOOLONG;
		return -1;
	}
	snprintf(out, PATH_MAX, "%s/.Trash", tp);
	if (lstat(out, &st) == 0 && S_ISDIR(st.st_mode) &&
	    (st.st_mode & S_ISVTX)) {
		snprintf(out, PATH_MAX, "%s/.Trash/%u", tp, uid);
		if (own_dir(out) == 0)
			return 0;
	}
	snprintf(out, PATH_MAX, "%s/.Trash-%u", tp, uid);
	return own_dir(out);
}

/* Move path into the trash directory t, recording `orig` as the original
 * path. info/NAME.trashinfo is created first (exclusively, which reserves
 * NAME), then the file goes to files/NAME. */
static int trash_into(const char *t, const char *path, const char *orig)
{
	static char url[3 * PATH_MAX + 1];
	char files[PATH_MAX], info[PATH_MAX], fp[PATH_MAX], ip[PATH_MAX];
	char name[NAME_MAX + 1], date[32];
	const char *base = strrchr(path, '/') + 1;
	snprintf(files, sizeof files, "%s/files", t);
	snprintf(info, sizeof info, "%s/info", t);
	if (own_dir(files) < 0 || own_dir(info) < 0)
		return -1;

	size_t o = 0;
	for (const unsigned char *s = (const unsigned char *)orig; *s; s++) {
		if ((*s < 0x80 && isalnum(*s)) || strchr("/-._~", *s))
			url[o++] = *s;
		else
			o += sprintf(url + o, "%%%02X", *s);
	}
	url[o] = 0;
	time_t now = time(NULL);
	struct tm tm;
	if (!localtime_r(&now, &tm) ||
	    !strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm))
		snprintf(date, sizeof date, "1970-01-01T00:00:00");

	/* room for ".trashinfo" and a ".NNN" counter */
	int keep = u8_fit(base, strlen(base), NAME_MAX - 10 - 4);
	for (int k = 1; k < 1000; k++) {
		if (k == 1)
			snprintf(name, sizeof name, "%.*s", keep, base);
		else
			snprintf(name, sizeof name, "%.*s.%d", keep, base, k);
		struct stat st;
		if (join(fp, files, name) < 0 ||
		    snprintf(ip, sizeof ip, "%s/%s.trashinfo", info, name) >=
		    (int)sizeof ip) {
			errno = ENAMETOOLONG;
			return -1;
		}
		int fd = open(ip, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0) {
			if (errno == EEXIST)
				continue;
			return -1;
		}
		if (lstat(fp, &st) == 0) {      /* left over: try another name */
			close(fd);
			unlink(ip);
			continue;
		}
		FILE *f = fdopen(fd, "w");
		int bad = !f || fprintf(f, "[Trash Info]\nPath=%s\n"
			"DeletionDate=%s\n", url, date) < 0;
		if (f ? fclose(f) != 0 : close(fd) != 0)
			bad = 1;
		if (bad || rename_new(path, fp) < 0) {
			int e = bad ? EIO : errno;
			unlink(ip);
			errno = e;
			return -1;
		}
		return 0;
	}
	errno = EEXIST;
	return -1;
}

/* Trash one entry. On failure, why it failed goes to err. */
static int trash_one(const char *path, char *err, size_t esz)
{
	char par[PATH_MAX], rpar[PATH_MAX], orig[PATH_MAX], t[PATH_MAX];
	char top[PATH_MAX];
	const char *base = strrchr(path, '/') + 1;
	uint64_t mnt, tm;
	snprintf(par, sizeof par, "%s", path);
	parent_of(par);
	if (!realpath(par, rpar) || mount_id(path, 0, &mnt) < 0)
		goto fail;
	if (in_trash(rpar)) {
		snprintf(err, esz, "%s is already in the trash", base);
		return -1;
	}
	if (is_mount(path)) {
		snprintf(err, esz, "%s is a mount point", base);
		return -1;
	}
	/* The physical path, which stays right when a link to it goes. */
	if (join(orig, rpar, base) < 0)
		goto fail;
	if (home_trash(t, 1) == 0 && mount_id(t, 1, &tm) == 0 && tm == mnt) {
		if (trash_into(t, path, orig) == 0)
			return 0;
		if (errno != EXDEV)
			goto fail;
	}
	if (topdir_trash(rpar, mnt, top, t) < 0)
		goto fail;
	/* In a topdir trash the path is relative to the top, so it still
	 * holds when the medium is mounted somewhere else. */
	const char *rel = orig + (strcmp(top, "/") ? strlen(top) : 0) + 1;
	if (trash_into(t, path, rel) == 0)
		return 0;
fail:
	snprintf(err, esz, "%s: %s", base, strerror(errno));
	return -1;
}

/* ---- copy, move, delete ------------------------------------------------- */

/* Run prog and wait for it, with stderr captured. 0 on success, else -1
 * and the first line of its error output (or a substitute) in msg. */
static int wait_prog(const char *prog, char *const argv[], char *msg,
	size_t msz)
{
	int p[2], st = 0;
	char buf[1024];
	size_t len = 0;
	if (pipe2(p, O_CLOEXEC) < 0) {
		snprintf(msg, msz, "%s: %s", argv[0], strerror(errno));
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		snprintf(msg, msz, "%s: %s", argv[0], strerror(errno));
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0)
			dup2(nul, 0);
		dup2(p[1], 2);
		execv(prog, argv);
		dprintf(2, "cannot run %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	close(p[1]);
	for (;;) {
		char junk[512];
		ssize_t r = len < sizeof buf - 1 ?
			read(p[0], buf + len, sizeof buf - 1 - len) :
			read(p[0], junk, sizeof junk);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		if (len < sizeof buf - 1)
			len += r;
	}
	close(p[0]);
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		return 0;
	buf[len] = 0;
	buf[strcspn(buf, "\n")] = 0;
	if (buf[0])
		snprintf(msg, msz, "%s", buf);
	else
		snprintf(msg, msz, "%s failed", argv[0]);
	return -1;
}

/* One failure line for mfm; a newline in a file name must not make two. */
static void report(int out, const char *fmt, ...)
{
	char b[1200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	for (char *s = b; *s; s++)
		if (*s == '\n')
			*s = '?';
	dprintf(out, "%s\n", b);
}

/* Run cp, mv, rm or chmod (looked up once) with up to four arguments. */
static int tool(int which, const char *a1, const char *a2, const char *a3,
	const char *a4, char *msg, size_t msz)
{
	static const char *const names[] = { "cp", "mv", "rm", "chmod" };
	static char path[4][PATH_MAX];
	static int err[4] = { -1, -1, -1, -1 };
	if (err[which] < 0)
		err[which] = find_prog(names[which], path[which]);
	if (err[which]) {
		snprintf(msg, msz, "cannot run %s: %s", names[which],
			strerror(err[which]));
		return -1;
	}
	char *argv[] = { (char *)names[which], (char *)a1, (char *)a2,
		(char *)a3, (char *)a4, NULL };
	return wait_prog(path[which], argv, msg, msz);
}

/*
 * The child that does a job, one item after the other. A move within a
 * file system is a rename. A copy, or a move to another file system, goes
 * to a hidden directory in the target first (cp -a / mv) and is renamed
 * into place from there, so a partial copy never shows under the real name
 * and the final name is picked only when it is used: nothing is ever
 * overwritten, not even on a case-insensitive file system. Delete is
 * rm -rf, after making sure there is no mount in the way. A NUL byte at the end
 * tells mfm that the job ran to its end.
 */
static void worker(int op, const char *sdir, char **names, int n,
	const char *ddir, int out)
{
	char src[PATH_MAX], tmp[PATH_MAX], t[PATH_MAX], msg[1024];
	char final[NAME_MAX + 1];
	setsid();               /* not killed with mfm's terminal */
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_IGN);       /* mfm may quit; finish the job */
	if (op == OP_SEND) {
		/* names is MFM_SEND's argv with the paths at its end */
		if (wait_prog(send_prog, names, msg, sizeof msg) < 0)
			report(out, "%s", msg);
		n = 0;
	}
	for (int i = 0; i < n; i++) {
		const char *nm = names[i];
		struct stat st;
		if (join(src, sdir, nm) < 0 || lstat(src, &st) < 0) {
			report(out, "%s: %s", nm, strerror(errno));
			continue;
		}
		int isdir = S_ISDIR(st.st_mode);
		if (isdir && op != OP_COPY && is_mount(src)) {
			report(out, "%s is a mount point, not %s", nm, op_ed[op]);
			continue;
		}
		if (op == OP_DELETE) {
			int below = isdir ? mount_below(src) : 0;
			if (below > 0)
				report(out, "%s has a mount point in it, not deleted",
					nm);
			else if (below < 0)
				report(out, "%s: cannot check for mount points: %s",
					nm, strerror(errno));
			else if (tool(2, "-rf", "--one-file-system", "--", src,
			    msg, sizeof msg) < 0)
				report(out, "%s", msg);
			continue;
		}
		if (op == OP_MOVE) {
			if (place_any(src, ddir, nm, isdir, 1, final) == 0)
				continue;
			/* EINVAL: into itself, which paste() rules out */
			if (errno != EXDEV) {
				report(out, "%s: %s", nm, strerror(errno));
				continue;
			}
			/* mv's removal after the copy would empty what is mounted. */
			int below = isdir ? mount_below(src) : 0;
			if (below != 0) {
				report(out, below > 0 ?
					"%s has a mount point in it, not moved" :
					"%s: cannot check for mount points, not moved",
					nm);
				continue;
			}
		}
		if (join(tmp, ddir, ".mfm-XXXXXX") < 0 || !mkdtemp(tmp)) {
			report(out, "%s: %s", nm, strerror(errno));
			continue;
		}
		if (join(t, tmp, nm) < 0) {
			report(out, "%s: %s", nm, strerror(errno));
			rmdir(tmp);
			continue;
		}
		int ok = op == OP_COPY ?
			tool(0, "-a", "--", src, t, msg, sizeof msg) == 0 :
			tool(1, "--", src, t, NULL, msg, sizeof msg) == 0;
		/* A failed mv may have removed part of the source already, so
		 * whatever arrived is kept, but not under the real name: it may
		 * be half a file. A failed copy is cleaned up, also from the
		 * read-only directories cp -a made. */
		if (op == OP_MOVE && !ok) {
			if (lstat(t, &st) < 0) {
				report(out, "%s", msg);
				rmdir(tmp);
			} else {
				report(out, "%s (what arrived is in %s)", msg,
					strrchr(tmp, '/') + 1);
			}
			continue;
		}
		if (op == OP_COPY && !ok) {
			char why[sizeof msg];
			snprintf(why, sizeof why, "%s", msg);
			tool(3, "-R", "u+w", "--", tmp, msg, sizeof msg);
			if (tool(2, "-rf", "--", tmp, NULL, msg, sizeof msg) < 0)
				report(out, "%s (the rest is in %s)", why,
					strrchr(tmp, '/') + 1);
			else
				report(out, "%s", why);
			continue;
		}
		if (place_any(t, ddir, nm, isdir, op == OP_MOVE, final) < 0) {
			report(out, "%s: %s (left in %s)", nm, strerror(errno),
				strrchr(tmp, '/') + 1);
			continue;
		}
		rmdir(tmp);
	}
	if (write(out, "", 1) < 0) {
		/* mfm has quit: nobody to tell */
	}
	_exit(0);
}

static int busy(void)
{
	if (job.fd < 0)
		return 0;
	set_status(1, "still %s, please wait", op_ing[job.op]);
	return 1;
}

/* Start op on the entries `names` of sdir, into ddir. `first` is selected
 * in ddir when it is done. */
static void start_job(int op, const char *sdir, char **names, int n,
	const char *ddir, const char *first)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0) {
		set_status(1, "cannot start %s: %s", op_ing[op], strerror(errno));
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		set_status(1, "cannot start %s: %s", op_ing[op], strerror(errno));
		close(p[0]);
		close(p[1]);
		return;
	}
	if (pid == 0) {
		/* Keep nothing of mfm but the pipe: holding on to the Wayland
		 * connection would keep the window up after mfm quits. */
		if (p[1] != 3 && dup2(p[1], 3) < 0)
			_exit(1);
		fcntl(3, F_SETFD, FD_CLOEXEC);
		close_range(4, ~0U, 0);
		worker(op, sdir, names, n, ddir, 3);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	job.fd = p[0];
	job.op = op;
	job.n = n;
	job.fails = 0;
	job.finished = 0;
	job.outlen = 0;
	job.clip_gen = clip_gen;
	snprintf(job.dir, sizeof job.dir, "%s", ddir);
	snprintf(job.first, sizeof job.first, "%s", first ? first : "");
	set_status(0, "%s %d item%s ...", op_ing[op], n, n == 1 ? "" : "s");
}

static void select_name(const char *name)
{
	for (int i = 0; i < nents; i++)
		if (!strcmp(ents[i].name, name)) {
			sel = i;
			show_sel();
			dirty = 1;
			return;
		}
}

static void clip_clear(void)
{
	free_names(clip, nclip);
	clip = NULL;
	nclip = 0;
	clip_gen++;
	dirty = 1;
}

static void job_done(void)
{
	close(job.fd);
	job.fd = -1;
	job.out[job.outlen] = 0;
	job.out[strcspn(job.out, "\n")] = 0;
	/* A move that worked used up the clipboard, unless it was replaced
	 * in the meantime. */
	if (job.op == OP_MOVE && job.finished && !job.fails &&
	    job.clip_gen == clip_gen)
		clip_clear();
	if (reload() == 0 && job.first[0] && !strcmp(cwd, job.dir))
		select_name(job.first);
	int ok = job.n - job.fails;
	if (!job.finished)
		set_status(1, "%s stopped unexpectedly, the result may be "
			"incomplete", op_ing[job.op]);
	else if (job.fails && job.op == OP_SEND)
		set_status(1, "sending failed: %s", job.out);
	else if (job.fails)
		set_status(1, "%d of %d failed: %s", job.fails, job.n, job.out);
	else
		set_status(0, "%s %d item%s%s", op_ed[job.op], ok,
			ok == 1 ? "" : "s",
			job.op == OP_DELETE ? " permanently" : "");
}

/* Read what the worker reports, without blocking. */
static void job_poll(void)
{
	char b[256];
	while (job.fd >= 0) {
		ssize_t r = read(job.fd, b, sizeof b);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (r <= 0) {
			job_done();
			return;
		}
		for (ssize_t i = 0; i < r; i++) {
			if (!b[i]) {
				job.finished = 1;
				continue;
			}
			if (job.outlen < sizeof job.out - 1)
				job.out[job.outlen++] = b[i];
			job.fails += b[i] == '\n';
		}
	}
}

/* Ctrl+C / Ctrl+X. The marks have done their job then. */
static void clip_set(int move)
{
	int n;
	char **v = targets(&n);
	if (!v)
		return;
	clip_clear();
	clip = v;
	nclip = n;
	clip_move = move;
	snprintf(clip_dir, sizeof clip_dir, "%s", cwd);
	unmark_all();
	if (n == 1)
		set_status(0, "%s \"%s\": Ctrl+V in the target directory",
			move ? "move" : "copy", clip[0]);
	else
		set_status(0, "%s %d items: Ctrl+V in the target directory",
			move ? "move" : "copy", n);
}

/* Ctrl+V: copy or move the clipboard into cwd. */
static void paste(void)
{
	char p[PATH_MAX];
	if (!nclip) {
		set_status(1, "nothing to paste: mark with Space, then "
			"Ctrl+C or Ctrl+X");
		return;
	}
	if (busy())
		return;
	if (clip_move && same_dir(clip_dir, cwd)) {
		set_status(1, "already in this directory");
		return;
	}
	for (int i = 0; i < nclip; i++) {
		struct stat st;
		if (join(p, clip_dir, clip[i]) < 0) {
			set_status(1, "%s: %s", clip[i], strerror(errno));
			return;
		}
		/* cp would fail, but only after copying one level */
		if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode) &&
		    inside(cwd, p)) {
			set_status(1, "%s: cannot %s a directory into itself",
				clip[i], clip_move ? "move" : "copy");
			return;
		}
	}
	start_job(clip_move ? OP_MOVE : OP_COPY, clip_dir, clip, nclip, cwd,
		clip[0]);
}

/* Ctrl+S: run MFM_SEND once with the full paths of the marked entries, else
 * of the selected one, as a job: in the background, with its exit status and
 * first error line in the status line. */
static void send_files(void)
{
	const char *cmd = getenv("MFM_SEND");
	char b[2048], *av[34], name[256];
	if (!cmd || !*cmd) {
		set_status(1, "nothing to send with: set MFM_SEND, e.g. "
			"MFM_SEND=adb-push-download");
		return;
	}
	if (busy())
		return;
	int ac = split_cmd(cmd, b, sizeof b, av, 33);
	if (ac <= 0) {
		set_status(1, "MFM_SEND: cannot parse the command");
		return;
	}
	snprintf(name, sizeof name, "%s", av[0]);
	int e = find_prog(av[0], send_prog);
	if (e) {
		set_status(1, "cannot run %s: %s", name, strerror(e));
		return;
	}
	int n;
	char **v = targets(&n);
	if (!v)
		return;
	char **args = calloc(ac + n + 1, sizeof *args);
	int ok = args != NULL;
	for (int i = 0; ok && i < ac; i++)
		ok = (args[i] = strdup(av[i])) != NULL;
	for (int i = 0; ok && i < n; i++) {
		char p[PATH_MAX];
		if (join(p, cwd, v[i]) < 0) {
			set_status(1, "%s: %s", v[i], strerror(errno));
			free_names(args, ac + n);
			free_names(v, n);
			return;
		}
		ok = (args[ac + i] = strdup(p)) != NULL;
	}
	if (!ok) {
		set_status(1, "%s", strerror(ENOMEM));
	} else {
		unmark_all();
		start_job(OP_SEND, cwd, args, n, cwd, NULL);
	}
	free_names(args, ac + n);
	free_names(v, n);
}

/* Delete: into the trash, at once. */
static void trash(void)
{
	char p[PATH_MAX], err[sizeof status_msg] = "", e[sizeof status_msg];
	int n, ok = 0;
	if (busy())
		return;
	char **v = targets(&n);
	if (!v)
		return;
	for (int i = 0; i < n; i++) {
		if (join(p, cwd, v[i]) < 0)
			snprintf(e, sizeof e, "%s: %s", v[i], strerror(errno));
		else if (trash_one(p, e, sizeof e) == 0) {
			ok++;
			continue;
		}
		if (!err[0])
			snprintf(err, sizeof err, "%s", e);
	}
	free_names(v, n);
	reload();
	if (ok < n)
		set_status(1, "%d of %d not moved to the trash: %s "
			"(Shift+Del deletes permanently)", n - ok, n, err);
	else
		set_status(0, "moved %d item%s to the trash", n,
			n == 1 ? "" : "s");
}

/* Shift+Delete: ask first. The question is drawn from pend, so no other
 * message can hide it. */
static void ask_delete(void)
{
	int n;
	if (busy())
		return;
	char **v = targets(&n);
	if (!v)
		return;
	free_names(pend, npend);
	pend = v;
	npend = n;
	snprintf(pend_dir, sizeof pend_dir, "%s", cwd);
	dirty = 1;
}

static void answer_delete(int yes)
{
	if (yes)
		start_job(OP_DELETE, pend_dir, pend, npend, pend_dir, NULL);
	else
		set_status(0, "nothing deleted");
	free_names(pend, npend);
	pend = NULL;
	npend = 0;
	dirty = 1;
}

/* ---- navigation --------------------------------------------------------- */

/* Absolute, normalized form of `target` (absolute or relative to cwd),
 * stat()ed into *st. -1 with errno set on failure. */
static int resolve(const char *target, char *out, struct stat *st)
{
	static char raw[2 * PATH_MAX + 2];
	int n;
	if (target[0] == '/')
		n = snprintf(raw, sizeof raw, "%s", target);
	else
		n = snprintf(raw, sizeof raw, "%s/%s", cwd, target);
	if (n < 0 || (size_t)n >= sizeof raw || normalize(raw, out) < 0) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (stat(out, st) == 0)
		return 0;
	/* "link/.." taken textually may not exist where the kernel's would
	 * be; fall back to the physical path, as cd -L does. */
	int e = errno;
	if (realpath(raw, out) && stat(out, st) == 0)
		return 0;
	errno = e;
	return -1;
}

/* Enter `target`. A file is opened if allow_open, else it is an error.
 * Returns 0 when listed, 1 when a file was opened, -1 (errno set). */
static int go_to(const char *target, const char *select, int allow_open)
{
	char p[PATH_MAX];
	struct stat st;
	search[0] = 0;
	if (resolve(target, p, &st) < 0) {
		set_status(1, "%s: %s", target, strerror(errno));
		return -1;
	}
	if (!S_ISDIR(st.st_mode)) {
		if (!allow_open) {
			set_status(1, "%s: %s", target, strerror(ENOTDIR));
			errno = ENOTDIR;
			return -1;
		}
		open_file(p);
		return 1;
	}
	status_msg[0] = 0;
	/* Arriving at a hidden entry (".config" when going up, "mfm .rc"):
	 * show hidden files, or the selection would silently be lost. */
	int reveal = select && select[0] == '.' && strcmp(select, "..") &&
		!show_hidden;
	if (reveal)
		show_hidden = 1;
	if (load(p, select, 0) < 0) {
		if (reveal)
			show_hidden = 0;
		return -1;
	}
	if (reveal)
		set_status(0, "hidden files shown (Ctrl+H hides them)");
	return 0;
}

/* Up one level; parents we may pass through but not list are skipped. */
static void go_parent(void)
{
	char p[PATH_MAX], child[NAME_MAX + 1], skipped[PATH_MAX] = "";
	snprintf(p, sizeof p, "%s", cwd);
	for (int first = 1; strcmp(p, "/"); first = 0) {
		const char *s = strrchr(p, '/');
		snprintf(child, sizeof child, "%s", s ? s + 1 : "");
		parent_of(p);
		if (go_to(first ? ".." : p, child, 0) == 0) {
			if (skipped[0])
				set_status(1, "skipped %s: %s", skipped,
					strerror(EACCES));
			return;
		}
		if (errno != EACCES && errno != EPERM)
			return;
		if (!skipped[0])
			snprintf(skipped, sizeof skipped, "%s", p);
	}
}

static int go_home(void)
{
	const char *h = getenv("HOME");
	return go_to(h && *h ? h : "/", NULL, 0);
}

static void activate(int i)
{
	char p[PATH_MAX];
	if (i < 0 || i >= nents)
		return;
	if (ents[i].up) {
		go_parent();
	} else if (ents[i].dir) {
		go_to(ents[i].name, NULL, 0);
	} else if (join(p, cwd, ents[i].name) < 0) {
		set_status(1, "%s: %s", ents[i].name, strerror(errno));
	} else {
		open_file(p);
	}
}

static void move_sel(int d)
{
	search[0] = 0;
	last_idx = -1;
	sel += d;
	show_sel();
	dirty = 1;
}

static char climb_failed[PATH_MAX];

/* cwd vanished or became unreadable: go to the nearest listable ancestor. */
static void climb(void)
{
	char p[PATH_MAX], from[PATH_MAX];
	struct stat st;
	snprintf(from, sizeof from, "%s", cwd);
	snprintf(p, sizeof p, "%s", cwd);
	while (strcmp(p, "/")) {
		parent_of(p);
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode) &&
		    load(p, NULL, 0) == 0) {
			set_status(1, "%s is no longer accessible", from);
			return;
		}
	}
	/* Nothing can be listed. Don't retry until the directory changes. */
	snprintf(climb_failed, sizeof climb_failed, "%s", cwd);
	if (stat(cwd, &st) == 0)
		cwd_mtime = st.st_mtim;
	cwd_unsure = 0;
}

/* Errors that mean the directory is gone for us, as opposed to a passing
 * shortage (EMFILE, ENOMEM, ...) that the next poll may get past. */
static int gone(int e)
{
	return e == ENOENT || e == ENOTDIR || e == EACCES || e == EPERM ||
		e == ELOOP || e == ENOTCONN || e == ESTALE;
}

static void poll_dir(void)
{
	struct stat st;
	/* Nothing above could be listed either: climb() stays put, and would
	 * try that again every second. */
	int stuck = !strcmp(cwd, climb_failed);
	if (stat(cwd, &st) != 0) {
		if (gone(errno))
			climb();
		return;
	}
	if (!S_ISDIR(st.st_mode)) {
		climb();
		return;
	}
	/* chmod does not change the mtime that is checked below. */
	if (access(cwd, R_OK | X_OK) != 0) {
		if (gone(errno) && !stuck)
			climb();
		return;
	}
	if (!cwd_unsure && st.st_mtim.tv_sec == cwd_mtime.tv_sec &&
	    st.st_mtim.tv_nsec == cwd_mtime.tv_nsec)
		return;
	if (reload() < 0 && gone(errno))
		climb();
}

static void typeahead(const char *txt, int n)
{
	size_t len = strlen(search);
	uint64_t now = now_ms();
	int fresh = len == 0 || now - search_ms >= TYPEAHEAD_MS;
	if (fresh)
		len = 0;
	if (len + n >= sizeof search)
		return;
	memcpy(search + len, txt, n);
	search[len + n] = 0;
	search_ms = now;
	last_idx = -1;
	int start = fresh ? sel + 1 : sel;
	for (int k = 0; k < nents; k++) {
		int i = (start + k) % nents;
		if (prefix_ci(ents[i].name, search)) {
			sel = i;
			show_sel();
			break;
		}
	}
	dirty = 1;
}

/* ---- text input ------------------------------------------------------- */

/*
 * One line of input with a cursor at byte *cur, shared by the path field
 * and the prompt. Returns 1 for Return, -1 for Escape, else 0.
 * While *fresh, the text before the cursor counts as selected: with
 * `sel`, typing replaces it and Backspace/Delete remove it; without (the
 * path field), only "/" or "~" start over. Only keys that change the text
 * or move the cursor end the fresh state; Shift, AltGr or a dead key
 * pressed on the way to "/" or "~" must not.
 */
static int line_key(char *b, size_t sz, size_t *cur, int *fresh, int sel,
	xkb_keysym_t ks, const char *txt, int n, int ctrl)
{
	size_t len = strlen(b), c = MIN(*cur, len);
	const char *p;
	int cut = *fresh && sel && c > 0;       /* a selection to replace */
	switch (ks) {
	case XKB_KEY_Return: case XKB_KEY_KP_Enter:
		return 1;
	case XKB_KEY_Escape:
		return -1;
	/* The cursor stays out of a letter and its combining marks. */
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
		while (c) {
			c = u8_prev(b, c);
			p = b + c;
			if (!is_combining(u8_next(&p)))
				break;
		}
		break;
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
		if (c < len) {
			p = b + c;
			u8_next(&p);
			for (const char *q = p; *q && is_combining(u8_next(&q));)
				p = q;
			c = p - b;
		}
		break;
	case XKB_KEY_Home: case XKB_KEY_KP_Home:
		c = 0;
		break;
	case XKB_KEY_End: case XKB_KEY_KP_End:
		c = len;
		break;
	case XKB_KEY_BackSpace:
		if (cut) {
			memmove(b, b + c, len - c + 1);
			c = 0;
		} else if (c) {
			size_t q = u8_prev(b, c);
			memmove(b + q, b + c, len - c + 1);
			c = q;
		}
		break;
	case XKB_KEY_Delete: case XKB_KEY_KP_Delete:
		if (cut) {
			memmove(b, b + c, len - c + 1);
			c = 0;
		} else if (c < len) {
			p = b + c;
			u8_next(&p);
			memmove(b + c, p, len - (p - b) + 1);
		}
		break;
	default:
		if (ctrl && ks == XKB_KEY_u) {
			b[0] = 0;
			c = 0;
			break;
		}
		if (ctrl || n <= 0 || (unsigned char)txt[0] < 0x20 ||
		    txt[0] == 0x7f)
			return 0;
		if (cut) {
			if (len - c + n >= sz)
				return 0;       /* would not fit: keep all */
			memmove(b, b + c, len - c + 1);
			len -= c;
			c = 0;
		} else if (*fresh && !sel && (txt[0] == '/' || txt[0] == '~')) {
			b[0] = 0;
			len = c = 0;
		}
		if (len + n >= sz)
			return 0;
		memmove(b + c + n, b + c, len - c + 1);
		memcpy(b + c, txt, n);
		c += n;
	}
	/* Deleting can join stray bytes into one character; keep the
	 * cursor on a character boundary. */
	for (p = b; (size_t)(p - b) < c;)
		u8_next(&p);
	*fresh = 0;
	*cur = p - b;
	dirty = 1;
	return 0;
}

static void edit_begin(void)
{
	editing = edit_fresh = 1;
	if ((size_t)snprintf(edit, sizeof edit, "%s%s", cwd,
	    strcmp(cwd, "/") ? "/" : "") >= sizeof edit)
		snprintf(edit, sizeof edit, "%s", cwd);
	edit_cur = strlen(edit);
	dirty = 1;
}

static void edit_commit(void)
{
	char t[PATH_MAX];
	editing = 0;
	dirty = 1;
	if (!edit[0])
		return;
	if (edit[0] == '~' && (edit[1] == 0 || edit[1] == '/')) {
		const char *home = getenv("HOME");
		if (!home || !*home) {
			set_status(1, "~: HOME is not set");
			return;
		}
		if ((size_t)snprintf(t, sizeof t, "%s%s", home, edit + 1) >=
		    sizeof t) {
			set_status(1, "%s: %s", edit, strerror(ENAMETOOLONG));
			return;
		}
		go_to(t, NULL, 1);
	} else {
		go_to(edit, NULL, 1);
	}
}

static void edit_key(xkb_keysym_t ks, const char *txt, int n, int ctrl)
{
	int r = line_key(edit, sizeof edit, &edit_cur, &edit_fresh, 0, ks,
		txt, n, ctrl);
	if (r > 0) {
		edit_commit();
	} else if (r < 0) {
		editing = 0;
		dirty = 1;
	}
}

/* ---- rename, new folder ------------------------------------------------- */

/* Is there an entry named exactly `name` in dir? (lstat can't tell on a
 * case-insensitive file system.) */
static int listed(const char *dir, const char *name)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	int found = 0;
	if (!d)
		return 0;
	while (!found && (de = readdir(d)))
		found = !strcmp(de->d_name, name);
	closedir(d);
	return found;
}

static int rename_lost;         /* the status line says where the file went */

/* Rename p to q in cwd by way of a temporary name. */
static int rename_via_tmp(const char *p, const char *q)
{
	char t[PATH_MAX], nm[64];
	int k;
	for (k = 0; k < 100; k++) {
		snprintf(nm, sizeof nm, ".mfm-rename-%d-%d", (int)getpid(), k);
		if (join(t, cwd, nm) < 0)
			return -1;
		if (rename_new(p, t) == 0)
			break;
		if (errno != EEXIST)
			return -1;
	}
	if (k == 100) {
		errno = EEXIST;
		return -1;
	}
	if (rename_new(t, q) == 0)
		return 0;
	int e = errno;
	/* Put it back, which must not replace what came up under the old name
	 * meanwhile. Failing that, say where it is. */
	if (rename_new(t, p) < 0) {
		rename_lost = 1;
		set_status(1, "%s is now %s", strrchr(p, '/') + 1, nm);
	}
	errno = e;
	return -1;
}

static void prompt_begin(int kind)
{
	if (busy())             /* the job may be working on that entry */
		return;
	if (kind == PR_RENAME) {
		if (sel < 0 || sel >= nents || ents[sel].up) {
			set_status(1, "nothing selected");
			return;
		}
		snprintf(pold, sizeof pold, "%s", ents[sel].name);
		snprintf(pbuf, sizeof pbuf, "%s", pold);
		/* The name without its extension is selected, as usual. */
		const char *dot = ents[sel].dir ? NULL : strrchr(pbuf, '.');
		pcur = dot && dot != pbuf ? (size_t)(dot - pbuf) : strlen(pbuf);
		pfresh = 1;
	} else {
		pbuf[0] = 0;
		pcur = 0;
		pfresh = 0;
	}
	prompt = kind;
	snprintf(pdir, sizeof pdir, "%s", cwd);
	search[0] = 0;
	clear_status();
	dirty = 1;
}

static void prompt_commit(void)
{
	char p[PATH_MAX], q[PATH_MAX];
	int kind = prompt;
	prompt = PR_NONE;
	dirty = 1;
	if (!pbuf[0] || (kind == PR_RENAME && !strcmp(pbuf, pold)))
		return;
	if (strcmp(pdir, cwd)) {        /* the directory went away meanwhile */
		set_status(1, "%s is gone, nothing %s", pdir,
			kind == PR_RENAME ? "renamed" : "created");
		return;
	}
	if (strchr(pbuf, '/') || !strcmp(pbuf, ".") || !strcmp(pbuf, "..")) {
		set_status(1, "not a valid name: %s", pbuf);
		return;
	}
	if (join(q, cwd, pbuf) < 0) {
		set_status(1, "%s: %s", pbuf, strerror(errno));
		return;
	}
	if (kind == PR_MKDIR) {
		if (mkdir(q, 0777) < 0) {
			set_status(1, "cannot create %s: %s", pbuf,
				strerror(errno));
			return;
		}
		reload();
		select_name(pbuf);
		set_status(0, "created %s", pbuf);
		return;
	}
	struct stat a, b;
	int r;
	if (join(p, cwd, pold) < 0) {
		r = -1;
	} else if (lstat(p, &a) == 0 && lstat(q, &b) == 0 &&
		   a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
		/* The "existing" target is the file itself. Either both
		 * names are hard links to one file, or only the case changes
		 * on a file system that ignores case (FAT). rename() does
		 * nothing in both cases, so the case change goes through a
		 * temporary name. */
		if (listed(cwd, pbuf)) {
			set_status(1, "%s and %s are the same file", pold,
				pbuf);
			return;
		}
		r = rename_via_tmp(p, q);
	} else {
		r = rename_new(p, q);
	}
	if (r < 0) {
		if (!rename_lost)
			set_status(1, "cannot rename %s: %s", pold, strerror(errno));
		rename_lost = 0;
		return;
	}
	reload();
	select_name(pbuf);
	set_status(0, "renamed %s to %s", pold, pbuf);
}

static void prompt_key(xkb_keysym_t ks, const char *txt, int n, int ctrl)
{
	int r = line_key(pbuf, sizeof pbuf, &pcur, &pfresh, 1, ks, txt, n,
		ctrl);
	if (r > 0) {
		prompt_commit();
	} else if (r < 0) {
		prompt = PR_NONE;
		dirty = 1;
	}
}

/* ---- drawing ------------------------------------------------------------ */

static void set_rgb(uint32_t c)
{
	cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0,
		((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static void fill(uint32_t px, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	set_rgb(px);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

/* The outline of a box from (x, y) to (x + w, y + h), both included. */
static void outline(uint32_t px, int x, int y, int w, int h)
{
	fill(px, x, y, w + 1, 1);
	fill(px, x, y + h, w + 1, 1);
	fill(px, x, y, 1, h + 1);
	fill(px, x + w, y, 1, h + 1);
}

static void bevel(struct rect r, int raised)
{
	uint32_t t = raised ? px_top : px_bot;
	uint32_t b = raised ? px_bot : px_top;
	for (int i = 0; i < BEV; i++) {
		fill(t, r.x + i, r.y + i, r.w - 2 * i, 1);
		fill(t, r.x + i, r.y + i, 1, r.h - 2 * i);
		fill(b, r.x + i + 1, r.y + r.h - 1 - i, r.w - 2 * i - 1, 1);
		fill(b, r.x + r.w - 1 - i, r.y + i + 1, 1, r.h - 2 * i - 1);
	}
}

/* textw/text take valid UTF-8 only; text_fit converts with disp(). */
static int textw(const char *s, int n)
{
	int w;
	if (n <= 0)
		return 0;
	pango_layout_set_text(plo, s, n);
	pango_layout_get_pixel_size(plo, &w, NULL);
	return w;
}

static void text(uint32_t c, int x, int y, int h, const char *s, int n)
{
	if (n <= 0)
		return;
	int base = y + (h - (font_asc + font_desc)) / 2 + font_asc;
	pango_layout_set_text(plo, s, n);
	set_rgb(c);
	cairo_move_to(cr, x, base - pango_layout_get_baseline(plo) / PANGO_SCALE);
	pango_cairo_show_layout(cr, plo);
}

/* A character boundary in s between lo and hi, not lo or hi themselves, near
 * the middle; -1 if there is none. */
static int mid_char(const char *s, int lo, int hi)
{
	int m = lo + (hi - lo) / 2;
	while (m > lo && (s[m] & 0xc0) == 0x80)
		m--;
	if (m == lo)
		for (m = lo + 1; m < hi && (s[m] & 0xc0) == 0x80; m++)
			;
	return m < hi ? m : -1;
}

/* Longest prefix of s[0..n) (in bytes, on a character boundary) that is
 * at most maxw wide. Shaping is too slow to go through it by the character
 * for every row. */
static int fit_prefix(const char *s, int n, int maxw)
{
	if (textw(s, n) <= maxw)
		return n;
	int lo = 0, hi = n, m;
	while ((m = mid_char(s, lo, hi)) >= 0) {
		if (textw(s, m) <= maxw)
			lo = m;
		else
			hi = m;
	}
	return lo;
}

/* Start of the longest suffix of s[0..n) that is at most maxw wide. */
static int fit_suffix(const char *s, int n, int maxw)
{
	if (textw(s, n) <= maxw)
		return 0;
	int lo = 0, hi = n, m;
	while ((m = mid_char(s, lo, hi)) >= 0) {
		if (textw(s + m, n - m) <= maxw)
			hi = m;
		else
			lo = m;
	}
	return hi;
}

/* Draw s in at most maxw pixels, shortened with "..." at the end or front.
 * Returns the width drawn. */
static int text_fit(uint32_t c, int x, int y, int h, int maxw,
	const char *raw, int cut_front)
{
	const char *s = disp(raw);
	int n = strlen(s), ew = textw("...", 3);
	if (maxw <= 0)
		return 0;
	if (fit_prefix(s, n, maxw) == n) {
		text(c, x, y, h, s, n);
		return textw(s, n);
	}
	if (maxw <= ew)
		return 0;
	if (!cut_front) {
		int k = fit_prefix(s, n, maxw - ew);
		text(c, x, y, h, s, k);
		text(c, x + textw(s, k), y, h, "...", 3);
		return textw(s, k) + ew;
	}
	int j = fit_suffix(s, n, maxw - ew);
	text(c, x, y, h, "...", 3);
	text(c, x + ew, y, h, s + j, n - j);
	return ew + textw(s + j, n - j);
}

/* An input line in r, scrolled so the cursor (at byte cur of the raw
 * text b) shows. With `selected`, the text before the cursor is drawn as
 * selected. */
static void draw_input(struct rect r, const char *b, size_t cur,
	int selected)
{
	static char pre[PATH_MAX];
	snprintf(pre, sizeof pre, "%.*s", (int)cur, b);
	int dc = strlen(disp(pre));
	const char *d = disp(b);
	int n = strlen(d);
	dc = MIN(dc, n);
	int ew = textw("...", 3), maxw = r.w - 2;
	int x = r.x, start = 0, end = n;
	if (maxw <= 2 * ew)
		return;
	if (fit_prefix(d, n, maxw) < n) {
		if (fit_prefix(d, dc, maxw - ew) < dc) {
			start = fit_suffix(d, dc, maxw - 2 * ew);
			text(xc_fg, x, r.y, r.h, "...", 3);
			x += ew;
		}
		end = start + fit_prefix(d + start, n - start,
			maxw - (x - r.x) - ew);
	}
	/* One layout for all that shows: cut at the cursor, right-to-left text
	 * would come apart. */
	int seg = end - start, c = dc - start;
	PangoRectangle at;
	pango_layout_set_text(plo, d + start, seg);
	pango_layout_get_cursor_pos(plo, c, &at, NULL);
	if (selected && c > 0) {
		int *ranges, nr;
		pango_layout_line_get_x_ranges(pango_layout_get_line_readonly(plo, 0),
			0, c, &ranges, &nr);
		for (int i = 0; i < nr; i++)
			fill(px_sel, x + PANGO_PIXELS(ranges[2 * i]), r.y + 2,
				PANGO_PIXELS(ranges[2 * i + 1] - ranges[2 * i]),
				r.h - 4);
		g_free(ranges);
		PangoAttrList *attrs = pango_attr_list_new();
		PangoAttribute *fg = pango_attr_foreground_new(
			((xc_sel >> 16) & 0xff) * 257, ((xc_sel >> 8) & 0xff) * 257,
			(xc_sel & 0xff) * 257);
		fg->start_index = 0;
		fg->end_index = c;
		pango_attr_list_insert(attrs, fg);
		pango_layout_set_attributes(plo, attrs);
		pango_attr_list_unref(attrs);
	}
	text(xc_fg, x, r.y, r.h, d + start, seg);
	pango_layout_set_attributes(plo, NULL);
	if (end < n)
		text(xc_fg, x + textw(d + start, seg), r.y, r.h, "...", 3);
	fill(px_fg, x + PANGO_PIXELS(at.x), r.y + 3, 1, r.h - 6);
}

static void draw_icon(int x, int y, const struct ent *e)
{
	int s = rowh - 6, iy = y + 3;
	if (e->dir) {
		fill(px_folder, x, iy + 1, s / 2, 2);
		fill(px_folder, x, iy + 3, s, s - 4);
		outline(px_bot, x, iy + 3, s - 1, s - 5);
	} else {
		fill(px_paper, x + 2, iy, s - 4, s);
		outline(px_bot, x + 2, iy, s - 5, s - 1);
		if (!e->nostat && (e->mode & 0111))
			fill(px_sel, x + 4, iy + 2, s - 8, 2);
	}
	if (e->link)
		fill(px_fg, x, iy + s - 3, 3, 3);
}

static void layout(void)
{
	int y0 = PAD + barh + 4;
	r_up = (struct rect){ PAD, PAD, barh, barh };
	r_path = (struct rect){ PAD + barh + 4, PAD,
		W - 2 * PAD - barh - 4, barh };
	r_status = (struct rect){ PAD, H - PAD - barh, W - 2 * PAD, barh };
	int y1 = r_status.y - 4;
	r_list = (struct rect){ PAD, y0, W - 2 * PAD - SBW - 2, y1 - y0 };
	r_sb = (struct rect){ r_list.x + r_list.w + 2, y0, SBW, y1 - y0 };
}

static void sb_geom(int *ty, int *th, int *sy, int *sh)
{
	int vis = rows_vis();
	*ty = r_sb.y + BEV;
	*th = r_sb.h - 2 * BEV;
	if (nents <= vis) {
		*sy = *ty;
		*sh = *th;
		return;
	}
	*sh = MAX(16, *th * vis / nents);
	*sy = *ty + (int)((long)(*th - *sh) * top / (nents - vis));
}

static void format_date(time_t t, char *s, size_t n)
{
	struct tm tm;
	/* localtime fails for times outside struct tm's year range, which
	 * tmpfs or btrfs happily store. */
	if (!localtime_r(&t, &tm) || !strftime(s, n, "%Y-%m-%d %H:%M", &tm))
		snprintf(s, n, "?");
}

static void paint(void)
{
	char s[sizeof status_msg + 32];
	fill(px_bg, 0, 0, W, H);

	/* parent button with an up arrow */
	bevel(r_up, !up_pressed);
	int cx = r_up.x + r_up.w / 2 + up_pressed, cy = r_up.y + r_up.h / 2;
	int a = r_up.h / 4;
	set_rgb(px_fg);
	cairo_move_to(cr, cx, cy - a);
	cairo_line_to(cr, cx - a - 1, cy + a / 2 + 1);
	cairo_line_to(cr, cx + a + 1, cy + a / 2 + 1);
	cairo_close_path(cr);
	cairo_fill(cr);
	fill(px_fg, cx - a / 3, cy + a / 2, 2 * (a / 3) + 1, a);

	/* path field */
	struct rect in_p = { r_path.x + BEV, r_path.y + BEV,
		r_path.w - 2 * BEV, r_path.h - 2 * BEV };
	fill(editing ? px_paper : px_list, in_p.x, in_p.y, in_p.w, in_p.h);
	bevel(r_path, 0);
	if (editing)
		draw_input((struct rect){ in_p.x + 4, in_p.y, in_p.w - 8,
			in_p.h }, edit, edit_cur, 0);
	else
		text_fit(xc_fg, in_p.x + 4, in_p.y, in_p.h, in_p.w - 10, cwd,
			1);

	/* list */
	int lx = r_list.x + BEV, ly = r_list.y + BEV + 2;
	int lw = r_list.w - 2 * BEV, vis = rows_vis();
	fill(px_list, lx, r_list.y + BEV, lw, r_list.h - 2 * BEV);
	bevel(r_list, 0);
	int datew = textw("0000-00-00 00:00", 16);
	int sizew = textw("0000.0 M", 8);
	int show_date = lw > 420;
	for (int i = top; i < nents && i < top + vis; i++) {
		const struct ent *e = &ents[i];
		int y = ly + (i - top) * rowh;
		uint32_t c = xc_fg, cd = xc_dim;
		if (i == sel) {
			fill(px_sel, lx + 1, y, lw - 2, rowh);
			c = cd = xc_sel;
		} else if (e->mark) {
			fill(px_mark, lx + 1, y, lw - 2, rowh);
		}
		if (e->mark)
			fill(i == sel ? px_mark : px_sel, lx + 1, y, 3, rowh);
		draw_icon(lx + 5, y, e);
		int x = lx + 5 + rowh + 2, right = lx + lw - 8;
		if (show_date) {
			if (!e->up) {
				if (e->nostat)
					snprintf(s, sizeof s, "?");
				else
					format_date(e->mtime, s, sizeof s);
				text(cd, right - textw(s, strlen(s)), y, rowh,
					s, strlen(s));
			}
			right -= datew + 14;
		}
		if (!e->dir && !e->nostat) {
			human(e->size, s, sizeof s);
			int n = strlen(s);
			text(cd, right - textw(s, n), y, rowh, s, n);
		}
		right -= sizew + 14;
		text_fit(c, x, y, rowh, right - x, e->name, 0);
	}

	/* scrollbar */
	int ty, th, sy, sh;
	sb_geom(&ty, &th, &sy, &sh);
	fill(px_trough, r_sb.x + BEV, ty, r_sb.w - 2 * BEV, th);
	bevel(r_sb, 0);
	struct rect sl = { r_sb.x + BEV, sy, r_sb.w - 2 * BEV, sh };
	fill(px_bg, sl.x, sl.y, sl.w, sl.h);
	bevel(sl, 1);

	/* status line */
	bevel(r_status, 0);
	struct rect in_s = { r_status.x + BEV + 4, r_status.y,
		r_status.w - 2 * BEV - 8, r_status.h };
	int rw = 0;
	if (prompt) {
		const char *l = prompt == PR_RENAME ? "Rename to: " :
			"New folder: ";
		int lw = textw(l, strlen(l));
		text(xc_fg, in_s.x, in_s.y, in_s.h, l, strlen(l));
		fill(px_paper, in_s.x + lw, in_s.y + BEV + 1, in_s.w - lw,
			in_s.h - 2 * BEV - 2);
		draw_input((struct rect){ in_s.x + lw + 3, in_s.y,
			in_s.w - lw - 3, in_s.h }, pbuf, pcur, pfresh);
		return;
	}
	if (!pend && !status_msg[0] && !search[0] && sel < nents &&
	    !ents[sel].up) {
		char p[11], sz[32];
		perms(&ents[sel], p);
		if (ents[sel].dir || ents[sel].nostat)
			snprintf(s, sizeof s, "%s", p);
		else {
			human(ents[sel].size, sz, sizeof sz);
			snprintf(s, sizeof s, "%s   %s", p, sz);
		}
		rw = textw(s, strlen(s));
		text(xc_dim, in_s.x + in_s.w - rw, in_s.y, in_s.h, s,
			strlen(s));
		rw += 16;
	}
	uint32_t sc = xc_fg;
	if (pend) {
		if (npend == 1)
			snprintf(s, sizeof s, "delete \"%s\" permanently? "
				"(y/n)", pend[0]);
		else
			snprintf(s, sizeof s, "delete %d items permanently? "
				"(y/n)", npend);
		sc = xc_err;
	} else if (status_msg[0]) {
		snprintf(s, sizeof s, "%s", status_msg);
		if (status_err)
			sc = xc_err;
	} else if (search[0]) {
		snprintf(s, sizeof s, "Find: %s", search);
	} else {
		int n = nents - (nents && ents[0].up), m = nmarked();
		size_t l = snprintf(s, sizeof s, "%d item%s", n,
			n == 1 ? "" : "s");
		if (m)
			l += snprintf(s + l, sizeof s - l, ", %d marked", m);
		if (sort_by || sort_rev)
			l += snprintf(s + l, sizeof s - l, ", %s",
				sort_desc[sort_by][sort_rev]);
		if (show_hidden)
			l += snprintf(s + l, sizeof s - l, ", hidden shown");
		if (nclip)
			l += snprintf(s + l, sizeof s - l, ", %d to %s", nclip,
				clip_move ? "move" : "copy");
		if (job.fd >= 0)
			snprintf(s + l, sizeof s - l, ", %s ...",
				op_ing[job.op]);
	}
	text_fit(sc, in_s.x, in_s.y, in_s.h, in_s.w - rw, s, 0);
}

/* 0 if the compositor holds both buffers: try again after a release. */
static int draw(void)
{
	struct shmbuf *b = wsurf_buffer(&main_s, W, H);
	if (!b)
		return 0;
	cr = b->cr;
	paint();
	wsurf_present(&main_s, b);
	return 1;
}

static void on_resize(void)
{
	int was = sel_visible();
	layout();
	if (was)
		show_sel();
	else
		clamp_top();
	dirty = 1;
}

/* ---- context menu ------------------------------------------------------- */

/*
 * The right-button menu: an xdg_popup that holds the grab while it is up.
 * The compositor still sends what happens over our own main window: to it
 * the menu is not special there, and one that keeps the pointer on the window
 * a button was pressed in sends the whole drag that way. So the pointer
 * handlers turn such positions into menu coordinates. Press, drag and release on
 * an item runs it; a quick click leaves the menu open for a second click.
 * Up/Down move through it, Return runs an item, Esc closes it.
 *
 * The listing may be reloaded while the menu is up (the directory poll, a
 * finished job). A command only runs if the directory, the clicked entry
 * and the number of marks are still what the menu was opened on.
 */
enum { M_OPEN, M_CUT, M_COPY, M_PASTE, M_RENAME, M_TRASH, M_DELETE,
	M_MKDIR, M_TERM, M_HIDDEN, M_RELOAD, M_SEND, M_SEP };
static const struct { int id; const char *label, *accel; } menu_items[] = {
	{ M_OPEN, "Open", "Enter" },
	{ M_SEP, NULL, NULL },
	{ M_CUT, "Cut", "Ctrl+X" },
	{ M_COPY, "Copy", "Ctrl+C" },
	{ M_PASTE, "Paste", "Ctrl+V" },
	{ M_SEND, "Send", "Ctrl+S" },     /* only with MFM_SEND */
	{ M_SEP, NULL, NULL },
	{ M_RENAME, "Rename", "F2" },
	{ M_TRASH, "Move to trash", "Del" },
	{ M_DELETE, "Delete permanently", "Shift+Del" },
	{ M_SEP, NULL, NULL },
	{ M_MKDIR, "New folder", "F7" },
	{ M_TERM, "Terminal here", "Ctrl+T" },
	{ M_HIDDEN, "Show hidden files", "Ctrl+H" },
	{ M_RELOAD, "Reload", "F5" },
};
#define NMENU ((int)(sizeof menu_items / sizeof menu_items[0]))
static struct wsurf menu_s;
static struct xdg_surface *menu_xdg;
static struct xdg_popup *menu_popup;
static int menu_configured;
static int menu_px, menu_py;    /* where the menu is, in the main window */
static int menu_on, menu_dirty, menu_hot = -1, menu_w, menu_h;
static int menu_presses, menu_entered;
static int menu_ok[NMENU], menu_y[NMENU + 1];  /* item i: y[i]..y[i+1] */
static uint32_t menu_time;
/* what the menu was opened on; menu_name is "" for the empty area */
static char menu_dir[PATH_MAX], menu_name[NAME_MAX + 1];
static int menu_nmarks;

static void toggle_hidden(void)
{
	show_hidden = !show_hidden;
	reload();
	set_status(0, "hidden files %s", show_hidden ? "shown" : "hidden");
}

static void set_sort(int by)
{
	sort_rev = by == sort_by ? !sort_rev : 0;
	sort_by = by;
	if (reload() < 0)
		return;         /* the old order stays, the error is shown */
	show_sel();
	set_status(0, "sorted %s", sort_desc[sort_by][sort_rev]);
}

static const char *menu_label(int i)
{
	if (menu_items[i].id == M_HIDDEN && show_hidden)
		return "Hide hidden files";
	return menu_items[i].label;
}

static int menu_inside(int x, int y)
{
	return x >= 0 && y >= 0 && x < menu_w && y < menu_h;
}

/* The item at (x, y) in the menu window, or -1. */
static int menu_at(int x, int y)
{
	if (x < BEV || x >= menu_w - BEV)
		return -1;
	for (int i = 0; i < NMENU; i++)
		if (y >= menu_y[i] && y < menu_y[i + 1])
			return menu_items[i].id == M_SEP ? -1 : i;
	return -1;
}

/* 0 if the compositor holds both buffers: try again after a release. */
static int menu_draw(void)
{
	struct shmbuf *b = wsurf_buffer(&menu_s, menu_w, menu_h);
	if (!b)
		return 0;
	cr = b->cr;
	fill(px_bg, 0, 0, menu_w, menu_h);
	bevel((struct rect){ 0, 0, menu_w, menu_h }, 1);
	for (int i = 0; i < NMENU; i++) {
		int y = menu_y[i], h = menu_y[i + 1] - y;
		if (!h)
			continue;       /* left out */
		if (menu_items[i].id == M_SEP) {
			fill(px_bot, BEV + 2, y + 3, menu_w - 2 * BEV - 4, 1);
			fill(px_top, BEV + 2, y + 4, menu_w - 2 * BEV - 4, 1);
			continue;
		}
		uint32_t c = menu_ok[i] ? xc_fg : xc_dim;
		if (i == menu_hot) {
			fill(px_sel, BEV, y, menu_w - 2 * BEV, h);
			c = xc_sel;
		}
		const char *l = menu_label(i), *a = menu_items[i].accel;
		text(c, BEV + 12, y, h, l, strlen(l));
		text(c, menu_w - BEV - 12 - textw(a, strlen(a)), y, h, a,
			strlen(a));
	}
	wsurf_present(&menu_s, b);
	return 1;
}

static void menu_close(void)
{
	if (!menu_on)
		return;
	if (ptr_surface == menu_s.wl)
		ptr_surface = NULL;
	xdg_popup_destroy(menu_popup);
	xdg_surface_destroy(menu_xdg);
	wsurf_destroy(&menu_s);
	/* The command that follows may block (a slow directory); the
	 * desktop must not stay grabbed meanwhile. */
	wl_display_flush(dpy);
	menu_on = 0;
	menu_hot = -1;
	dirty = 1;
}

static void popup_configure(void *data, struct xdg_popup *p, int32_t x,
	int32_t y, int32_t w, int32_t h)
{
	menu_px = x;
	menu_py = y;
}

/* A click on another client, or the compositor refused the grab. */
static void popup_done(void *data, struct xdg_popup *p)
{
	menu_close();
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
};

static void menu_xdg_configure(void *data, struct xdg_surface *s,
	uint32_t serial)
{
	xdg_surface_ack_configure(s, serial);
	menu_configured = 1;
	menu_dirty = 1;
}

static const struct xdg_surface_listener menu_xdg_listener = {
	menu_xdg_configure,
};

/* Which items work: `row` is the clicked entry, or -1 for the empty area
 * of the list. Commands on entries take the marks, else the selection. */
static int has_send(void)
{
	const char *s = getenv("MFM_SEND");
	return s && *s;
}

static void menu_enable(int row)
{
	int nm = nmarked(), idle = job.fd < 0;
	int entry = row >= 0 && !ents[row].up;
	int some = entry || (row < 0 && nm);
	for (int i = 0; i < NMENU; i++) {
		switch (menu_items[i].id) {
		/* the clicked entry alone, so not while others are marked */
		case M_OPEN: menu_ok[i] = row >= 0 && nm <= 1; break;
		case M_RENAME: menu_ok[i] = entry && nm <= 1 && idle; break;
		case M_CUT: case M_COPY: menu_ok[i] = some; break;
		case M_TRASH: case M_DELETE: menu_ok[i] = some && idle; break;
		case M_PASTE: menu_ok[i] = nclip > 0 && idle; break;
		case M_MKDIR: menu_ok[i] = idle; break;
		case M_SEND: menu_ok[i] = some && idle && has_send(); break;
		default: menu_ok[i] = 1;
		}
	}
}

/* Open the menu at the window position (px, py) for list row `row` (-1: the
 * empty area), with the button press that asks for it. */
static void menu_open(int px, int py, uint32_t serial, uint32_t t, int row)
{
	int ih = rowh + 4, aw = 0;
	int lw = textw("Hide hidden files", strlen("Hide hidden files"));
	menu_y[0] = BEV + 2;
	for (int i = 0; i < NMENU; i++) {
		int sep = menu_items[i].id == M_SEP;
		int gone = menu_items[i].id == M_SEND && !has_send();
		menu_y[i + 1] = menu_y[i] + (gone ? 0 : sep ? 8 : ih);
		if (sep || gone)
			continue;
		lw = MAX(lw, textw(menu_items[i].label,
			strlen(menu_items[i].label)));
		aw = MAX(aw, textw(menu_items[i].accel,
			strlen(menu_items[i].accel)));
	}
	int w = 2 * (BEV + 12) + lw + 32 + aw, h = menu_y[NMENU] + BEV + 2;
	menu_enable(row);
	menu_w = w;
	menu_h = h;
	/* Below and right of the pointer, else above or left of it with the
	 * pointer still on the corner, and always on the screen. */
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm_base);
	xdg_positioner_set_size(pos, w, h);
	xdg_positioner_set_anchor_rect(pos, MAX(0, MIN(px, W - 1)),
		MAX(0, MIN(py, H - 1)), 1, 1);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(pos,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
	menu_s.wl = wl_compositor_create_surface(compositor);
	menu_xdg = xdg_wm_base_get_xdg_surface(wm_base, menu_s.wl);
	xdg_surface_add_listener(menu_xdg, &menu_xdg_listener, NULL);
	menu_popup = xdg_surface_get_popup(menu_xdg, main_xdg, pos);
	xdg_popup_add_listener(menu_popup, &popup_listener, NULL);
	xdg_positioner_destroy(pos);
	xdg_popup_grab(menu_popup, seat, serial);
	wl_surface_commit(menu_s.wl);
	menu_configured = 0;
	menu_px = px;
	menu_py = py;
	snprintf(menu_dir, sizeof menu_dir, "%s", cwd);
	snprintf(menu_name, sizeof menu_name, "%s",
		row >= 0 ? ents[row].name : "");
	menu_nmarks = nmarked();
	menu_on = 1;
	menu_hot = -1;
	menu_presses = 0;
	menu_entered = 0;
	menu_time = t;
	menu_dirty = 1;
}

/* Is the listing still the one the menu was opened on? */
static int menu_valid(void)
{
	if (strcmp(cwd, menu_dir))
		return 0;
	if (menu_name[0] && (sel < 0 || sel >= nents ||
	    strcmp(ents[sel].name, menu_name)))
		return 0;
	return nmarked() == menu_nmarks;
}

static void menu_run(int i)
{
	int id = menu_items[i].id;
	menu_close();
	search[0] = 0;
	clear_status();
	if (id != M_HIDDEN && id != M_RELOAD && !menu_valid()) {
		set_status(1, "the listing changed, nothing done");
		return;
	}
	switch (id) {
	case M_OPEN: activate(sel); break;
	case M_CUT: clip_set(1); break;
	case M_COPY: clip_set(0); break;
	case M_PASTE: paste(); break;
	case M_SEND: send_files(); break;
	case M_RENAME: prompt_begin(PR_RENAME); break;
	case M_TRASH: trash(); break;
	case M_DELETE: ask_delete(); break;
	case M_MKDIR: prompt_begin(PR_MKDIR); break;
	case M_TERM: open_terminal(); break;
	case M_HIDDEN: toggle_hidden(); break;
	case M_RELOAD: reload(); break;
	}
	dirty = 1;
}

static void menu_key(xkb_keysym_t ks)
{
	int d;
	switch (ks) {
	case XKB_KEY_Escape:
		menu_close();
		return;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter:
		if (menu_hot >= 0)
			menu_run(menu_hot);
		return;
	case XKB_KEY_Up: case XKB_KEY_KP_Up: d = -1; break;
	case XKB_KEY_Down: case XKB_KEY_KP_Down: d = 1; break;
	default: return;
	}
	int i = menu_hot < 0 ? (d > 0 ? -1 : NMENU) : menu_hot;
	for (int k = 0; k < NMENU; k++) {
		i = (i + d + NMENU) % NMENU;
		if (menu_items[i].id != M_SEP && menu_ok[i]) {
			menu_hot = i;
			menu_dirty = 1;
			return;
		}
	}
}

/* The pointer highlights the enabled item under it. Until it first
 * reaches one, a nudge does not drop an item chosen with the keys. */
static void menu_motion(int x, int y)
{
	int i = menu_at(x, y);
	if (i >= 0 && !menu_ok[i])
		i = -1;
	if (i >= 0)
		menu_entered = 1;
	else if (!menu_entered)
		return;
	if (i != menu_hot) {
		menu_hot = i;
		menu_dirty = 1;
	}
}

static void menu_press(const struct pev *ev)
{
	if (ev->button > B_RIGHT)
		return;
	if (menu_inside(ev->x, ev->y))
		menu_presses++;
	else
		menu_close();
}

/* A release runs the highlighted item under the pointer: one that was
 * pointed at, not one the menu merely opened on top of. */
static void menu_release(const struct pev *ev)
{
	if (ev->button > B_RIGHT)
		return;
	/* the release of a click that opened the menu: it stays (the
	 * compositor's millisecond clock wraps at 32 bits) */
	if (!menu_presses && (uint32_t)(ev->time - menu_time) < MENU_CLICK_MS)
		return;
	int i = menu_at(ev->x, ev->y);
	if (i >= 0 && i == menu_hot)
		menu_run(i);
	else if (!menu_inside(ev->x, ev->y))
		menu_close();
}

/* ---- events ------------------------------------------------------------- */

static int is_modifier(xkb_keysym_t ks)
{
	return (ks >= XKB_KEY_Shift_L && ks <= XKB_KEY_Hyper_R) ||
		(ks >= XKB_KEY_ISO_Lock && ks <= XKB_KEY_ISO_Level5_Lock) ||
		ks == XKB_KEY_Mode_switch || ks == XKB_KEY_Num_Lock;
}

static void key(xkb_keysym_t ks, const char *txt, int n, unsigned state)
{
	if (is_modifier(ks))
		return;
	if (ks >= XKB_KEY_A && ks <= XKB_KEY_Z)
		ks += XKB_KEY_a - XKB_KEY_A;
	int ctrl = state & MOD_CTRL, alt = state & MOD_ALT;
	int shift = state & MOD_SHIFT;

	if (menu_on) {
		menu_key(ks);
		return;
	}
	if (editing) {
		edit_key(ks, txt, n, ctrl);
		return;
	}
	if (prompt) {
		prompt_key(ks, txt, n, ctrl);
		return;
	}
	if (pend) {
		answer_delete(!ctrl && !alt && (ks == XKB_KEY_y || ks == XKB_KEY_j));
		return;
	}
	/* A type-ahead that timed out is over: Backspace means "parent". */
	if (search[0] && now_ms() - search_ms >= TYPEAHEAD_MS)
		search[0] = 0;
	int had_msg = status_msg[0] != 0;
	clear_status();
	if (ctrl) {
		switch (ks) {
		case XKB_KEY_q: case XKB_KEY_w: exit(0);
		case XKB_KEY_h: toggle_hidden(); break;
		case XKB_KEY_1: case XKB_KEY_KP_1: set_sort(SORT_NAME); break;
		case XKB_KEY_2: case XKB_KEY_KP_2: set_sort(SORT_SIZE); break;
		case XKB_KEY_3: case XKB_KEY_KP_3: set_sort(SORT_DATE); break;
		case XKB_KEY_r: reload(); break;
		case XKB_KEY_l: edit_begin(); break;
		case XKB_KEY_t: open_terminal(); break;
		case XKB_KEY_a: mark_all(); break;
		case XKB_KEY_c: clip_set(0); break;
		case XKB_KEY_x: clip_set(1); break;
		case XKB_KEY_v: paste(); break;
		case XKB_KEY_s: send_files(); break;
		case XKB_KEY_n:
			if (shift)
				prompt_begin(PR_MKDIR);
			break;
		}
		return;
	}
	if (alt) {
		if (ks == XKB_KEY_Up || ks == XKB_KEY_KP_Up)
			go_parent();
		else if (ks == XKB_KEY_Home || ks == XKB_KEY_KP_Home)
			go_home();
		else if (ks == XKB_KEY_F4)
			exit(0);
		return;
	}
	int vis = rows_vis();
	switch (ks) {
	case XKB_KEY_Up: case XKB_KEY_KP_Up: move_sel(-1); return;
	case XKB_KEY_Down: case XKB_KEY_KP_Down: move_sel(1); return;
	case XKB_KEY_Prior: case XKB_KEY_KP_Prior: move_sel(-vis); return;
	case XKB_KEY_Next: case XKB_KEY_KP_Next: move_sel(vis); return;
	case XKB_KEY_Home: case XKB_KEY_KP_Home: move_sel(-nents); return;
	case XKB_KEY_End: case XKB_KEY_KP_End: move_sel(nents); return;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: activate(sel); return;
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
		if (sel < nents && ents[sel].dir)
			activate(sel);
		return;
	case XKB_KEY_Left: case XKB_KEY_KP_Left: go_parent(); return;
	case XKB_KEY_BackSpace:
		if (search[0]) {
			search[u8_prev(search, strlen(search))] = 0;
			search_ms = now_ms();
			dirty = 1;
		} else {
			go_parent();
		}
		return;
	case XKB_KEY_Escape:         /* one thing at a time */
		if (search[0])
			search[0] = 0;
		else if (!had_msg)
			unmark_all();
		dirty = 1;
		return;
	case XKB_KEY_F5: reload(); return;
	case XKB_KEY_F2: prompt_begin(PR_RENAME); return;
	case XKB_KEY_F7: prompt_begin(PR_MKDIR); return;
	case XKB_KEY_Insert: case XKB_KEY_KP_Insert:
		toggle_mark(sel);
		move_sel(1);
		return;
	case XKB_KEY_Delete: case XKB_KEY_KP_Delete:
		if (shift)
			ask_delete();
		else
			trash();
		return;
	case XKB_KEY_space:
		if (!search[0]) {       /* else part of a type-ahead name */
			toggle_mark(sel);
			move_sel(1);
			return;
		}
		break;
	}
	if (n > 0 && (unsigned char)txt[0] >= 0x20 && txt[0] != 0x7f)
		typeahead(txt, n);
}

/* The entry at window height y in the list, or -1. */
static int row_at(int y)
{
	int i = top + (y - (r_list.y + BEV + 2)) / rowh;
	if (y < r_list.y + BEV + 2 || i >= nents || i >= top + rows_vis())
		return -1;
	return i;
}

/* Right click in the list. A marked entry keeps the marks, so the menu
 * works on all of them; any other entry is selected alone. Not while the
 * left button drags the scrollbar or holds the up button. */
static void list_menu(const struct pev *ev)
{
	int i = row_at(ev->y);
	if (!in(r_list, ev->x, ev->y) ||
	    (ev->state & (BTN1_HELD | BTN2_HELD)))
		return;
	search[0] = 0;
	clear_status();
	if (i >= 0) {
		if (!ents[i].mark)
			unmark_all();
		sel = i;
		last_idx = -1;
		dirty = 1;
	}
	menu_open(ev->x, ev->y, ev->serial, ev->time, i);
}

static void on_button(const struct pev *ev)
{
	int x = ev->x, y = ev->y;
	if (pend)
		answer_delete(0);
	if (prompt) {
		prompt = PR_NONE;
		dirty = 1;
	}
	if (editing && !in(r_path, x, y)) {
		editing = 0;
		dirty = 1;
	}
	switch (ev->button) {
	case B_WHEEL_UP: top -= 3; clamp_top(); dirty = 1; return;
	case B_WHEEL_DOWN: top += 3; clamp_top(); dirty = 1; return;
	case B_BACK: go_parent(); return;
	case B_RIGHT: list_menu(ev); return;
	case B_LEFT: break;
	default: return;
	}
	if (in(r_up, x, y)) {
		up_pressed = 1;
		dirty = 1;
	} else if (in(r_path, x, y)) {
		if (!editing)
			edit_begin();
	} else if (in(r_sb, x, y)) {
		int ty, th, sy, sh;
		sb_geom(&ty, &th, &sy, &sh);
		if (y >= sy && y < sy + sh) {
			dragging = 1;
			drag_off = y - sy;
		} else {
			top += y < sy ? -rows_vis() : rows_vis();
			clamp_top();
		}
		dirty = 1;
	} else if (in(r_list, x, y)) {
		int i = row_at(y);
		if (i < 0)
			return;
		search[0] = 0;
		clear_status();
		if (ev->state & MOD_CTRL) {
			toggle_mark(i);
			sel = i;
			last_idx = -1;
		} else if (ev->state & MOD_SHIFT) {
			for (int k = MIN(sel, i); k <= MAX(sel, i); k++)
				if (!ents[k].up)
					ents[k].mark = 1;
			sel = i;
			last_idx = -1;
		/* A double click needs both clicks on the same, still
		 * selected row of the same listing. A plain click drops the
		 * marks, so a Delete after it hits what was clicked. */
		} else if (nmarked()) {
			unmark_all();
			sel = i;
			last_idx = i;
			last_click = ev->time;
		} else if (i == last_idx && i == sel &&
		    (uint32_t)(ev->time - last_click) < DCLICK_MS) {
			last_idx = -1;
			activate(i);
		} else {
			sel = i;
			last_idx = i;
			last_click = ev->time;
		}
		dirty = 1;
	}
}

static void on_motion(int y)
{
	if (!dragging)
		return;
	int ty, th, sy, sh, vis = rows_vis();
	sb_geom(&ty, &th, &sy, &sh);
	if (th > sh && nents > vis) {
		top = (int)((long)(y - drag_off - ty) * (nents - vis) /
			(th - sh));
		clamp_top();
		dirty = 1;
	}
}

static void on_release(const struct pev *ev)
{
	if (ev->button != B_LEFT)
		return;
	dragging = 0;
	if (up_pressed) {
		up_pressed = 0;
		dirty = 1;
		if (in(r_up, ev->x, ev->y))
			go_parent();
	}
}

/* ---- Wayland input ------------------------------------------------------ */

static unsigned current_state(void)
{
	unsigned s = btn_mask;
	if (kb_state) {
		if (xkb_state_mod_name_is_active(kb_state, XKB_MOD_NAME_SHIFT,
		    XKB_STATE_MODS_EFFECTIVE) > 0)
			s |= MOD_SHIFT;
		if (xkb_state_mod_name_is_active(kb_state, XKB_MOD_NAME_CTRL,
		    XKB_STATE_MODS_EFFECTIVE) > 0)
			s |= MOD_CTRL;
		if (xkb_state_mod_name_is_active(kb_state, XKB_MOD_NAME_ALT,
		    XKB_STATE_MODS_EFFECTIVE) > 0)
			s |= MOD_ALT;
	}
	return s;
}

/* A held key moves and types, but runs no command again: Delete would empty
 * the folder into the trash, Return would open a file 25 times a second. */
static int key_repeats(uint32_t kc)
{
	xkb_keysym_t ks = xkb_state_key_get_one_sym(kb_state, kc);
	unsigned state = current_state();
	if (pend)
		return 0;
	switch (ks) {
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
		return !(state & (MOD_CTRL | MOD_ALT));
	}
	if (menu_on || (state & (MOD_CTRL | MOD_ALT)))
		return 0;
	switch (ks) {
	case XKB_KEY_Prior: case XKB_KEY_KP_Prior:
	case XKB_KEY_Next: case XKB_KEY_KP_Next:
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
	case XKB_KEY_BackSpace:
		return 1;
	case XKB_KEY_Delete: case XKB_KEY_KP_Delete:
		return editing || prompt;
	}
	uint32_t c = xkb_keysym_to_utf32(ks);
	return c >= 0x20 && c != 0x7f;
}

/* One key press or repeat, with kc an xkb keycode. Compose and dead keys come
 * from xkbcommon; a key that is only part of a sequence does nothing else. */
static void key_press(uint32_t kc)
{
	char txt[64];
	int n = -1;
	xkb_keysym_t ks = xkb_state_key_get_one_sym(kb_state, kc);
	if (compose && xkb_compose_state_feed(compose, ks) ==
	    XKB_COMPOSE_FEED_ACCEPTED) {
		switch (xkb_compose_state_get_status(compose)) {
		case XKB_COMPOSE_COMPOSING:
			return;
		case XKB_COMPOSE_CANCELLED:
			xkb_compose_state_reset(compose);
			return;
		case XKB_COMPOSE_COMPOSED:
			n = xkb_compose_state_get_utf8(compose, txt, sizeof txt);
			ks = xkb_compose_state_get_one_sym(compose);
			xkb_compose_state_reset(compose);
			break;
		case XKB_COMPOSE_NOTHING:
			break;
		}
	}
	if (n < 0)
		n = xkb_state_key_get_utf8(kb_state, kc, txt, sizeof txt);
	if (n >= (int)sizeof txt)
		n = 0;
	key(ks, txt, n, current_state());
}

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format,
	int32_t fd, uint32_t size)
{
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *s = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (s == MAP_FAILED)
		return;
	struct xkb_keymap *map = xkb_keymap_new_from_string(xkb, s,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(s, size);
	struct xkb_state *state = map ? xkb_state_new(map) : NULL;
	if (!state) {
		xkb_keymap_unref(map);
		return;
	}
	xkb_state_unref(kb_state);
	xkb_keymap_unref(kb_map);
	kb_state = state;
	kb_map = map;
	repeat_kc = 0;
}

static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *surface, struct wl_array *keys)
{
}

static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *surface)
{
	repeat_kc = 0;
	if (compose)
		xkb_compose_state_reset(compose);
	/* What is held now is unknown, and a click can come before the next
	 * enter, which brings the real state. */
	if (kb_state)
		xkb_state_update_mask(kb_state, 0, 0, 0, 0, 0,
			xkb_state_serialize_layout(kb_state,
				XKB_STATE_LAYOUT_EFFECTIVE));
}

static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	uint32_t kc = code + 8;
	if (!kb_state)
		return;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (kc == repeat_kc)
			repeat_kc = 0;
		return;
	}
	/* A modifier does not stop a key that is repeating. */
	int repeats = xkb_keymap_key_repeats(kb_map, kc);
	if (repeats)
		repeat_kc = 0;
	key_press(kc);
	if (repeats && repeat_rate > 0 && key_repeats(kc)) {
		repeat_kc = kc;
		repeat_next = now_ms() + repeat_delay;
	}
}

static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (kb_state)
		xkb_state_update_mask(kb_state, depressed, latched, locked, 0, 0,
			group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *k, int32_t rate,
	int32_t delay)
{
	repeat_rate = rate;
	repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

/* The pointer position in menu coordinates, wherever it is. */
static void menu_pos(int *x, int *y)
{
	*x = ptr_x;
	*y = ptr_y;
	if (ptr_surface != menu_s.wl) {
		*x -= menu_px;
		*y -= menu_py;
	}
}

static void menu_pointer_moved(void)
{
	int x, y;
	menu_pos(&x, &y);
	menu_motion(x, y);
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	ptr_surface = surface;
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	/* An unchanged position gets no motion event after this. */
	if (menu_on)
		menu_pointer_moved();
	if (cursor_img) {
		wl_pointer_set_cursor(p, serial, cursor_surf, cursor_img->hotspot_x,
			cursor_img->hotspot_y);
		wl_surface_attach(cursor_surf,
			wl_cursor_image_get_buffer(cursor_img), 0, 0);
		wl_surface_damage_buffer(cursor_surf, 0, 0, cursor_img->width,
			cursor_img->height);
		wl_surface_commit(cursor_surf);
	}
}

/* Not every compositor keeps the pointer on a window while a button is held,
 * and then the release never reaches us: a drag ends at the window's edge. */
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *surface)
{
	ptr_surface = NULL;
	btn_mask = 0;
	dragging = 0;
	if (menu_on)            /* or Return would run what it left */
		menu_motion(-1, -1);
	if (up_pressed) {
		up_pressed = 0;
		dirty = 1;
	}
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
	wl_fixed_t x, wl_fixed_t y)
{
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	if (menu_on)
		menu_pointer_moved();
	else
		on_motion(ptr_y);
}

static void pointer_event(int button, int pressed, uint32_t serial,
	uint32_t time)
{
	if (!ptr_surface)
		return;
	struct pev e = { button, ptr_x, ptr_y, serial, time, current_state() };
	if (menu_on) {          /* the pointer belongs to the menu */
		menu_pos(&e.x, &e.y);
		if (pressed)
			menu_press(&e);
		else
			menu_release(&e);
	} else if (pressed) {
		on_button(&e);
	} else {
		on_release(&e);
	}
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	int pressed = state == WL_POINTER_BUTTON_STATE_PRESSED, b;
	unsigned held = 0;
	switch (code) {
	case BTN_LEFT: b = B_LEFT; held = BTN1_HELD; break;
	case BTN_MIDDLE: b = B_MIDDLE; held = BTN2_HELD; break;
	case BTN_RIGHT: b = B_RIGHT; break;
	case BTN_SIDE: case BTN_BACK: b = B_BACK; break;
	default: return;
	}
	pointer_event(b, pressed, serial, time);
	/* A press sees the buttons held before it, as with any toolkit. */
	if (pressed)
		btn_mask |= held;
	else
		btn_mask &= ~held;
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_acc += wl_fixed_to_double(value);
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_acc = 0;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
	int32_t steps)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_steps += steps;
}

/* Wheels report steps; touchpads only a distance, of which AXIS_STEP makes
 * one wheel step. */
static void ptr_frame(void *data, struct wl_pointer *p)
{
	enum { AXIS_STEP = 15 };
	int steps = axis_steps;
	if (steps) {
		axis_acc = 0;
	} else {
		steps = (int)(axis_acc / AXIS_STEP);
		axis_acc -= steps * AXIS_STEP;
	}
	axis_steps = 0;
	for (int i = 0; i < abs(steps); i++)
		pointer_event(steps < 0 ? B_WHEEL_UP : B_WHEEL_DOWN, 1, 0, 0);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	int want_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
	int want_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (want_ptr && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	} else if (!want_ptr && pointer) {
		wl_pointer_release(pointer);
		pointer = NULL;
		ptr_surface = NULL;
	}
	if (want_kb && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	} else if (!want_kb && keyboard) {
		wl_keyboard_release(keyboard);
		keyboard = NULL;
		repeat_kc = 0;
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- window setup ------------------------------------------------------- */

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_ping };

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			MIN(ver, 4));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 7));
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
			MIN(ver, 2));
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
		deco_mgr = wl_registry_bind(r, name,
			&zxdg_decoration_manager_v1_interface, 1);
	}
}

static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void main_xdg_configure(void *data, struct xdg_surface *s,
	uint32_t serial)
{
	xdg_surface_ack_configure(s, serial);
	int w = pend_w > 0 ? pend_w : float_w, h = pend_h > 0 ? pend_h : float_h;
	if (w != W || h != H) {
		W = w;
		H = h;
		on_resize();
	}
	if (!pend_bound) {
		float_w = W;
		float_h = H;
	}
	configured = 1;
	need_commit = 1;
	dirty = 1;
}

static const struct xdg_surface_listener main_xdg_listener = {
	main_xdg_configure,
};

/* 0 leaves the size to us, which after maximizing is the size from before. */
static void top_configure(void *data, struct xdg_toplevel *t, int32_t w,
	int32_t h, struct wl_array *states)
{
	uint32_t *st;
	pend_w = w;
	pend_h = h;
	pend_bound = 0;
	wl_array_for_each(st, states)
		if (*st != XDG_TOPLEVEL_STATE_ACTIVATED &&
		    *st != XDG_TOPLEVEL_STATE_RESIZING)
			pend_bound = 1;
}

static void top_close(void *data, struct xdg_toplevel *t)
{
	exit(0);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = top_configure,
	.close = top_close,
};

static void cursor_init(void)
{
	const char *s = getenv("XCURSOR_SIZE");
	int size = s && atoi(s) > 0 ? atoi(s) : 24;
	struct wl_cursor_theme *theme =
		wl_cursor_theme_load(getenv("XCURSOR_THEME"), size, shm);
	if (!theme)
		return;
	struct wl_cursor *c = wl_cursor_theme_get_cursor(theme, "default");
	if (!c)
		c = wl_cursor_theme_get_cursor(theme, "left_ptr");
	if (!c)
		return;
	cursor_img = c->images[0];
	cursor_surf = wl_compositor_create_surface(compositor);
}

/* The locale compose sequences follow, in the order of xkbcommon's docs. */
static void compose_init(void)
{
	const char *loc = getenv("LC_ALL");
	if (!loc || !*loc)
		loc = getenv("LC_CTYPE");
	if (!loc || !*loc)
		loc = getenv("LANG");
	if (!loc || !*loc)
		loc = "C";
	struct xkb_compose_table *table = xkb_compose_table_new_from_locale(xkb,
		loc, XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (!table)
		return;
	compose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
	xkb_compose_table_unref(table);
}

static void setup_window(void)
{
	main_s.wl = wl_compositor_create_surface(compositor);
	main_xdg = xdg_wm_base_get_xdg_surface(wm_base, main_s.wl);
	xdg_surface_add_listener(main_xdg, &main_xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(main_xdg);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, "mfm");
	xdg_toplevel_set_min_size(toplevel, 300, 200);
	if (deco_mgr) {
		struct zxdg_toplevel_decoration_v1 *d =
			zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr,
				toplevel);
		zxdg_toplevel_decoration_v1_set_mode(d,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	layout();
}

/* Where to start: the argument (a file is shown selected in its
 * directory), else the current directory. $PWD is used for that when it
 * is normalized and names the directory we are in, which keeps a
 * symlinked start directory logical. Failures fall back to $HOME and /,
 * and the reason stays in the status line. */
static void start(const char *arg)
{
	char p[PATH_MAX], err[sizeof status_msg] = "";
	struct stat a, b, st;
	const char *pwd = getenv("PWD");
	int ok = 0;

	if (pwd && pwd[0] == '/' && strlen(pwd) < sizeof cwd &&
	    normalize(pwd, p) == 0 && !strcmp(p, pwd) &&
	    stat(pwd, &a) == 0 && stat(".", &b) == 0 &&
	    a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
		snprintf(cwd, sizeof cwd, "%s", pwd);
	} else if (!getcwd(cwd, sizeof cwd)) {
		snprintf(err, sizeof err,
			"cannot determine the current directory: %s",
			strerror(errno));
		cwd[0] = 0;
	}

	if (arg && (arg[0] == '/' || cwd[0])) {
		if (resolve(arg, p, &st) < 0) {
			snprintf(err, sizeof err, "%s: %s", arg, strerror(errno));
		} else if (S_ISDIR(st.st_mode)) {
			ok = go_to(p, NULL, 0) == 0;
		} else {
			char base[NAME_MAX + 1];
			snprintf(base, sizeof base, "%s", strrchr(p, '/') + 1);
			parent_of(p);
			ok = go_to(p, base, 0) == 0;
		}
		if (!ok && !err[0])
			snprintf(err, sizeof err, "%s", status_msg);
	} else if (!arg && cwd[0]) {
		snprintf(p, sizeof p, "%s", cwd);
		ok = go_to(p, NULL, 0) == 0;
		if (!ok)
			snprintf(err, sizeof err, "%s", status_msg);
	}
	if (!ok && go_home() != 0 && go_to("/", NULL, 0) != 0) {
		snprintf(cwd, sizeof cwd, "/");
		set_status(1, "cannot list any directory");
		return;
	}
	if (err[0])
		set_status(1, "%s", err);
}

static void lost_connection(void)
{
	fprintf(stderr, "mfm: lost the connection to the compositor: %s\n",
		strerror(wl_display_get_error(dpy)));
	exit(1);
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "-v")) {
		puts("mfm " MFM_VERSION);
		return 0;
	}
	if (argc > 2 || (argc == 2 && (!strcmp(argv[1], "-h") ||
	    !strcmp(argv[1], "--help")))) {
		fprintf(stderr, "usage: mfm [-v] [directory or file]\n"
			"Environment: MFM_OPEN (default xdg-open), MFM_SEND, "
			"MFM_TERMINAL / MWC_TERMINAL,\n"
			"MFM_FONT (default \"Sans 10\"), "
			"MWC_INACTIVE, MWC_ACTIVE.\n");
		return argc == 2 ? 0 : 1;
	}
	setlocale(LC_ALL, "");
	signal(SIGCHLD, SIG_IGN);

	if (!(dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "mfm: cannot connect to a Wayland compositor\n");
		return 1;
	}
	if (!(xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		fprintf(stderr, "mfm: cannot set up xkbcommon\n");
		return 1;
	}
	compose_init();
	struct wl_registry *registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "mfm: the compositor lacks wl_compositor, wl_shm "
			"or xdg_wm_base\n");
		return 1;
	}
	cursor_init();
	theme_init();
	setup_window();
	start(argc == 2 ? argv[1] : NULL);
	set_title();
	wl_surface_commit(main_s.wl);

	int dfd = wl_display_get_fd(dpy);
	uint64_t next_poll = now_ms() + POLL_MS;
	for (;;) {
		if (wl_display_dispatch_pending(dpy) < 0)
			lost_connection();
		/* Timers run on every pass, so a steady stream of events
		 * (held keys, scrolling) cannot starve them. */
		uint64_t now = now_ms();
		job_poll();
		if (now >= next_poll) {
			poll_dir();
			next_poll = now + POLL_MS;
		}
		if (search[0] && now - search_ms >= TYPEAHEAD_MS) {
			search[0] = 0;
			dirty = 1;
		}
		if (repeat_kc && now >= repeat_next) {
			/* The key may mean something else by now. */
			if (key_repeats(repeat_kc))
				key_press(repeat_kc);
			else
				repeat_kc = 0;
			repeat_next = now_ms() + MAX(1000 / MAX(repeat_rate, 1), 1);
		}
		/* A frame at a time, except that a configure wants its commit
		 * even if the window is hidden and gets no frames. */
		if (dirty && configured && (!main_s.frame || need_commit) && draw()) {
			dirty = 0;
			need_commit = 0;
		}
		if (menu_on && menu_dirty && menu_configured && !menu_s.frame &&
		    menu_draw())
			menu_dirty = 0;

		if (wl_display_prepare_read(dpy) != 0)
			continue;       /* events arrived in the meantime */
		short dev = POLLIN;
		if (wl_display_flush(dpy) < 0) {
			if (errno != EAGAIN) {
				wl_display_cancel_read(dpy);
				lost_connection();
			}
			dev |= POLLOUT;
		}
		uint64_t wake = next_poll;
		if (search[0])
			wake = MIN(wake, search_ms + TYPEAHEAD_MS);
		if (repeat_kc)
			wake = MIN(wake, repeat_next);
		now = now_ms();
		struct pollfd fds[2] = {
			{ .fd = dfd, .events = dev },
			{ .fd = job.fd, .events = POLLIN },     /* -1: ignored */
		};
		poll(fds, 2, wake > now ? (int)MIN(wake - now, INT_MAX) : 0);
		if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(dpy) < 0)
				lost_connection();
		} else {
			wl_display_cancel_read(dpy);
		}
	}
}
