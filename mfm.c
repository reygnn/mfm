/*
 * mfm - a minimal file manager for X11, in the Motif look of mwm
 * (https://github.com/reygnn/mwm-x11).
 *
 * Plain Xlib + Xft, one file, no toolkit. It reads the same colours as mwm:
 * MWM_INACTIVE / MWM_ACTIVE from the environment, else Mwm*background and
 * Mwm*activeBackground from ~/.config/mwm/mwmrc, else the Motif defaults.
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
 * $MFM_TERMINAL, else $MWM_TERMINAL, else the first one found of xterm,
 * uxterm, x-terminal-emulator, ... Both commands are split at blanks
 * (quotes group words; no other shell syntax), and the program is looked
 * up in $PATH, never in the directory being browsed.
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#define BEV 2           /* bevel width, as in mwm */
#define PAD 6
#define SBW 15          /* scrollbar width */
#define DCLICK_MS 400
#define TYPEAHEAD_MS 1000
#define POLL_MS 1000
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

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
static Time last_click;
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
enum { OP_COPY, OP_MOVE, OP_DELETE };
static const char *const op_ing[] = { "copying", "moving", "deleting" };
static const char *const op_ed[] = { "copied", "moved", "deleted" };
static struct {
	int fd, op, n, fails;
	unsigned clip_gen;      /* the clipboard a move started from */
	char dir[PATH_MAX], first[NAME_MAX + 1];
	char out[512];
	size_t outlen;
} job = { .fd = -1 };

/* ---- X ------------------------------------------------------------------ */

static const long EVENT_MASK = ExposureMask | KeyPressMask | ButtonPressMask |
	ButtonReleaseMask | Button1MotionMask | StructureNotifyMask |
	FocusChangeMask;

static Display *dpy;
static int scr, depth;
static Window root, win;
static Visual *vis;
static Colormap cmap;
static GC gc;
static Pixmap buf;
static XftDraw *xd;
static XftFont *font;
static XIM xim;
static XIC xic;
static int focused;
static int W = 680, H = 480;
static Atom a_protocols, a_delete, a_ping, a_name, a_icon_name, a_utf8,
	a_icon, a_pid;
static unsigned long px_bg, px_top, px_bot, px_list, px_sel, px_trough,
	px_fg, px_folder, px_paper, px_mark;
static XftColor xc_fg, xc_sel, xc_dim, xc_err;
static int rowh, barh;
static struct rect r_up, r_path, r_list, r_sb, r_status;

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

/* A valid UTF-8 copy of s for drawing (static buffer): Latin-1 bytes are
 * converted, C1 control bytes become U+FFFD. */
static const char *disp(const char *s)
{
	static char out[3 * PATH_MAX + 8];
	size_t o = 0;
	while (*s && o + 4 < sizeof out) {
		uint32_t cp;
		int n = u8_len((const unsigned char *)s, &cp);
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

static int parse_color(const char *s, uint32_t *out)
{
	XColor c;
	if (!s || !*s || !XParseColor(dpy, cmap, s, &c))
		return 0;
	*out = (c.red >> 8) << 16 | (c.green >> 8) << 8 | (c.blue >> 8);
	return 1;
}

/* Last global "Mwm*key: value" line of ~/.config/mwm/mwmrc. */
static char *mwmrc_get(const char *key, char *val, size_t n)
{
	char path[PATH_MAX], line[512];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	int len;
	if (xdg && *xdg)
		len = snprintf(path, sizeof path, "%s/mwm/mwmrc", xdg);
	else
		len = snprintf(path, sizeof path, "%s/.config/mwm/mwmrc",
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
		if (strncmp(p, "Mwm*", 4) || strncmp(p + 4, key, kl) ||
		    p[4 + kl] != ':')
			continue;
		p += 5 + kl;
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

/* If the colormap is full, fall back to black or white, whichever is
 * closer, so text keeps its contrast. */
static unsigned long pixel(uint32_t rgb)
{
	XColor c = {
		.red = ((rgb >> 16) & 0xff) * 257,
		.green = ((rgb >> 8) & 0xff) * 257,
		.blue = (rgb & 0xff) * 257,
		.flags = DoRed | DoGreen | DoBlue,
	};
	if (XAllocColor(dpy, cmap, &c))
		return c.pixel;
	return is_light(rgb) ? WhitePixel(dpy, scr) : BlackPixel(dpy, scr);
}

static void xftcolor(XftColor *out, uint32_t rgb)
{
	XRenderColor rc = { ((rgb >> 16) & 0xff) * 257,
		((rgb >> 8) & 0xff) * 257, (rgb & 0xff) * 257, 0xffff };
	if (!XftColorAllocValue(dpy, vis, cmap, &rc, out) &&
	    !XftColorAllocName(dpy, vis, cmap,
		    is_light(rgb) ? "white" : "black", out)) {
		fprintf(stderr, "mfm: cannot allocate colours\n");
		exit(1);
	}
}

static void theme_init(void)
{
	uint32_t bg = setting("MWM_INACTIVE", "background", 0xaeb2c3);
	uint32_t act = setting("MWM_ACTIVE", "activeBackground", 0x7387ad);
	uint32_t fg = contrast(bg);

	px_bg = pixel(bg);
	px_top = pixel(mix(bg, 0xffffff, 0.55f));
	px_bot = pixel(mix(bg, 0x000000, 0.5f));
	px_list = pixel(mix(bg, 0xffffff, 0.3f));
	px_trough = pixel(mix(bg, 0x000000, 0.12f));
	px_sel = pixel(act);
	px_mark = pixel(mix(mix(bg, 0xffffff, 0.3f), act, 0.4f));
	px_fg = pixel(fg);
	px_folder = pixel(0xc9a85c);
	px_paper = pixel(0xf6f6f6);
	xftcolor(&xc_fg, fg);
	xftcolor(&xc_sel, contrast(act));
	xftcolor(&xc_dim, mix(bg, fg, 0.6f));
	xftcolor(&xc_err, 0x8a0303);

	const char *f = getenv("MFM_FONT");
	font = XftFontOpenName(dpy, scr, f && *f ? f : "Sans:size=10");
	if (!font)
		font = XftFontOpenName(dpy, scr, "fixed");
	if (!font) {
		fprintf(stderr, "mfm: cannot open a font\n");
		exit(1);
	}
	rowh = font->ascent + font->descent + 4;
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
	return strcoll(x->name, y->name);
}

static void free_ents(struct ent *v, int n)
{
	for (int i = 0; i < n; i++)
		free(v[i].name);
	free(v);
}

/* WM_NAME as STRING or COMPOUND_TEXT, _NET_WM_NAME as UTF8_STRING. */
static void set_name(int icon, Atom net, const char *utf8)
{
	XTextProperty tp;
	char *list[] = { (char *)utf8 };
	if (Xutf8TextListToTextProperty(dpy, list, 1, XStdICCTextStyle,
	    &tp) >= Success) {
		if (icon)
			XSetWMIconName(dpy, win, &tp);
		else
			XSetWMName(dpy, win, &tp);
		XFree(tp.value);
	}
	XChangeProperty(dpy, win, net, a_utf8, 8, PropModeReplace,
		(const unsigned char *)utf8, strlen(utf8));
}

static void set_title(void)
{
	static char t[sizeof "mfm: " + 3 * PATH_MAX + 8];
	snprintf(t, sizeof t, "mfm: %s", disp(cwd));
	set_name(0, a_name, t);
	const char *base = strrchr(t, '/');
	set_name(1, a_icon_name, base && base[1] ? base + 1 : t + 5);
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
	if (win)
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
	static const char *const terms[] = { "xterm", "uxterm",
		"x-terminal-emulator", "urxvt", "alacritty", "konsole",
		"kitty", "xfce4-terminal", "gnome-terminal" };
	const char *var = "MFM_TERMINAL", *t = getenv(var);
	char p[PATH_MAX], name[256];
	if (!t || !*t)
		t = getenv(var = "MWM_TERMINAL");
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

/* Run cp, mv or rm (looked up once) with up to four arguments. */
static int tool(int which, const char *a1, const char *a2, const char *a3,
	const char *a4, char *msg, size_t msz)
{
	static const char *const names[] = { "cp", "mv", "rm" };
	static char path[3][PATH_MAX];
	static int err[3] = { -1, -1, -1 };
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
 * rm -rf, which neither enters nor removes other mounts.
 */
static void worker(int op, const char *sdir, char **names, int n,
	const char *ddir, int out)
{
	char src[PATH_MAX], tmp[PATH_MAX], t[PATH_MAX], msg[1024];
	char final[NAME_MAX + 1];
	setsid();               /* not killed with mfm's terminal */
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_IGN);       /* mfm may quit; finish the job */
	for (int i = 0; i < n; i++) {
		const char *nm = names[i];
		struct stat st;
		if (join(src, sdir, nm) < 0 || lstat(src, &st) < 0) {
			report(out, "%s: %s", nm, strerror(errno));
			continue;
		}
		int isdir = S_ISDIR(st.st_mode);
		if (op == OP_DELETE) {
			if (isdir && is_mount(src))
				report(out, "%s is a mount point, not deleted",
					nm);
			else if (tool(2, "-rf", "--one-file-system", "--", src,
			    msg, sizeof msg) < 0)
				report(out, "%s", msg);
			continue;
		}
		if (op == OP_MOVE) {
			if (place(src, ddir, nm, isdir, 1, final) == 0)
				continue;
			/* EINVAL: into itself, which paste() rules out */
			if (errno != EXDEV) {
				report(out, "%s: %s", nm, strerror(errno));
				continue;
			}
		}
		if (join(tmp, ddir, ".mfm-XXXXXX") < 0 || !mkdtemp(tmp) ||
		    join(t, tmp, nm) < 0) {
			report(out, "%s: %s", nm, strerror(errno));
			continue;
		}
		int ok = op == OP_COPY ?
			tool(0, "-a", "--", src, t, msg, sizeof msg) == 0 :
			tool(1, "--", src, t, NULL, msg, sizeof msg) == 0;
		/* A failed mv may have removed part of the source already:
		 * whatever arrived is kept. A failed copy is cleaned up. */
		if (op == OP_MOVE && !ok && lstat(t, &st) < 0) {
			report(out, "%s", msg);
			rmdir(tmp);
			continue;
		}
		if (op == OP_COPY && !ok) {
			report(out, "%s", msg);
			tool(2, "-rf", "--", tmp, NULL, msg, sizeof msg);
			continue;
		}
		if (place(t, ddir, nm, isdir, op == OP_MOVE, final) < 0) {
			report(out, "%s: %s (left in %s)", nm, strerror(errno),
				strrchr(tmp, '/') + 1);
			continue;
		}
		rmdir(tmp);
		if (!ok)
			report(out, "%s (moved only partly)", msg);
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
		/* Keep nothing of mfm but the pipe: holding on to the X
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
	if (job.op == OP_MOVE && !job.fails && job.clip_gen == clip_gen)
		clip_clear();
	if (reload() == 0 && job.first[0] && !strcmp(cwd, job.dir))
		select_name(job.first);
	int ok = job.n - job.fails;
	if (job.fails)
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
	if (stat(cwd, &st) == 0)
		cwd_mtime = st.st_mtim;
	cwd_unsure = 0;
}

/* Errors that mean the directory is gone for us, as opposed to a passing
 * shortage (EMFILE, ENOMEM, ...) that the next poll may get past. */
static int gone(int e)
{
	return e == ENOENT || e == ENOTDIR || e == EACCES || e == EPERM ||
		e == ELOOP;
}

static void poll_dir(void)
{
	struct stat st;
	if (stat(cwd, &st) != 0) {
		if (gone(errno))
			climb();
		return;
	}
	if (!S_ISDIR(st.st_mode)) {
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
	KeySym ks, const char *txt, int n, int ctrl)
{
	size_t len = strlen(b), c = MIN(*cur, len);
	const char *p;
	int cut = *fresh && sel && c > 0;       /* a selection to replace */
	switch (ks) {
	case XK_Return: case XK_KP_Enter:
		return 1;
	case XK_Escape:
		return -1;
	case XK_Left: case XK_KP_Left:
		if (c)
			c = u8_prev(b, c);
		break;
	case XK_Right: case XK_KP_Right:
		if (c < len) {
			p = b + c;
			u8_next(&p);
			c = p - b;
		}
		break;
	case XK_Home: case XK_KP_Home:
		c = 0;
		break;
	case XK_End: case XK_KP_End:
		c = len;
		break;
	case XK_BackSpace:
		if (cut) {
			memmove(b, b + c, len - c + 1);
			c = 0;
		} else if (c) {
			size_t q = u8_prev(b, c);
			memmove(b + q, b + c, len - c + 1);
			c = q;
		}
		break;
	case XK_Delete: case XK_KP_Delete:
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
		if (ctrl && ks == XK_u) {
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

static void edit_key(KeySym ks, const char *txt, int n, int ctrl)
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
		/* The "existing" target is the file itself: only the case
		 * changes on a file system that ignores it (the old name is
		 * gone from the listing afterwards), or both names are hard
		 * links to one file, and rename() leaves both alone. */
		r = rename(p, q);
		if (r == 0 && listed(cwd, pold)) {
			set_status(1, "%s and %s are the same file", pold,
				pbuf);
			return;
		}
	} else {
		r = rename_new(p, q);
	}
	if (r < 0) {
		set_status(1, "cannot rename %s: %s", pold, strerror(errno));
		return;
	}
	reload();
	select_name(pbuf);
	set_status(0, "renamed %s to %s", pold, pbuf);
}

static void prompt_key(KeySym ks, const char *txt, int n, int ctrl)
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

static void fill(unsigned long px, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	XSetForeground(dpy, gc, px);
	XFillRectangle(dpy, buf, gc, x, y, w, h);
}

static void bevel(struct rect r, int raised)
{
	unsigned long t = raised ? px_top : px_bot;
	unsigned long b = raised ? px_bot : px_top;
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
	XGlyphInfo g;
	XftTextExtentsUtf8(dpy, font, (const FcChar8 *)s, n, &g);
	return g.xOff;
}

static void text(XftColor *c, int x, int y, int h, const char *s, int n)
{
	int base = y + (h - (font->ascent + font->descent)) / 2 + font->ascent;
	XftDrawStringUtf8(xd, c, font, x, base, (const FcChar8 *)s, n);
}

/* Longest prefix of s[0..n) (in bytes, on a character boundary) that is
 * at most maxw wide. Measures only about as much text as fits: Xft
 * reports widths as 16-bit values, which a whole long path overflows. */
static int fit_prefix(const char *s, int n, int maxw)
{
	int k = 0;
	while (k < n) {
		int next = k + 1;
		while (next < n && (s[next] & 0xc0) == 0x80)
			next++;
		if (textw(s, next) > maxw)
			break;
		k = next;
	}
	return k;
}

/* Start of the longest suffix of s[0..n) that is at most maxw wide. */
static int fit_suffix(const char *s, int n, int maxw)
{
	int j = n;
	while (j > 0) {
		int prev = j - 1;
		while (prev > 0 && (s[prev] & 0xc0) == 0x80)
			prev--;
		if (textw(s + prev, n - prev) > maxw)
			break;
		j = prev;
	}
	return j;
}

/* Draw s in at most maxw pixels, shortened with "..." at the end or front.
 * Returns the width drawn. */
static int text_fit(XftColor *c, int x, int y, int h, int maxw,
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
			text(&xc_fg, x, r.y, r.h, "...", 3);
			x += ew;
		}
		end = start + fit_prefix(d + start, n - start,
			maxw - (x - r.x) - ew);
	}
	int wsel = textw(d + start, dc - start);
	if (selected && dc > start)
		fill(px_sel, x, r.y + 2, wsel, r.h - 4);
	text(selected ? &xc_sel : &xc_fg, x, r.y, r.h, d + start, dc - start);
	if (end > dc)
		text(&xc_fg, x + wsel, r.y, r.h, d + dc, end - dc);
	if (end < n)
		text(&xc_fg, x + textw(d + start, end - start), r.y, r.h,
			"...", 3);
	fill(px_fg, x + wsel, r.y + 3, 1, r.h - 6);
}

static void draw_icon(int x, int y, const struct ent *e)
{
	int s = rowh - 6, iy = y + 3;
	if (e->dir) {
		fill(px_folder, x, iy + 1, s / 2, 2);
		fill(px_folder, x, iy + 3, s, s - 4);
		XSetForeground(dpy, gc, px_bot);
		XDrawRectangle(dpy, buf, gc, x, iy + 3, s - 1, s - 5);
	} else {
		fill(px_paper, x + 2, iy, s - 4, s);
		XSetForeground(dpy, gc, px_bot);
		XDrawRectangle(dpy, buf, gc, x + 2, iy, s - 5, s - 1);
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

static void draw(void)
{
	char s[sizeof status_msg + 32];
	fill(px_bg, 0, 0, W, H);

	/* parent button with an up arrow */
	bevel(r_up, !up_pressed);
	int cx = r_up.x + r_up.w / 2 + up_pressed, cy = r_up.y + r_up.h / 2;
	int a = r_up.h / 4;
	XPoint tri[3] = { { cx, cy - a }, { cx - a - 1, cy + a / 2 + 1 },
		{ cx + a + 1, cy + a / 2 + 1 } };
	XSetForeground(dpy, gc, px_fg);
	XFillPolygon(dpy, buf, gc, tri, 3, Convex, CoordModeOrigin);
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
		text_fit(&xc_fg, in_p.x + 4, in_p.y, in_p.h, in_p.w - 10, cwd,
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
		XftColor *c = &xc_fg, *cd = &xc_dim;
		if (i == sel) {
			fill(px_sel, lx + 1, y, lw - 2, rowh);
			c = cd = &xc_sel;
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
		text(&xc_fg, in_s.x, in_s.y, in_s.h, l, strlen(l));
		fill(px_paper, in_s.x + lw, in_s.y + BEV + 1, in_s.w - lw,
			in_s.h - 2 * BEV - 2);
		draw_input((struct rect){ in_s.x + lw + 3, in_s.y,
			in_s.w - lw - 3, in_s.h }, pbuf, pcur, pfresh);
		XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
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
		text(&xc_dim, in_s.x + in_s.w - rw, in_s.y, in_s.h, s,
			strlen(s));
		rw += 16;
	}
	XftColor *sc = &xc_fg;
	if (pend) {
		if (npend == 1)
			snprintf(s, sizeof s, "delete \"%s\" permanently? "
				"(y/n)", pend[0]);
		else
			snprintf(s, sizeof s, "delete %d items permanently? "
				"(y/n)", npend);
		sc = &xc_err;
	} else if (status_msg[0]) {
		snprintf(s, sizeof s, "%s", status_msg);
		if (status_err)
			sc = &xc_err;
	} else if (search[0]) {
		snprintf(s, sizeof s, "Find: %s", search);
	} else {
		int n = nents - (nents && ents[0].up), m = nmarked();
		size_t l = snprintf(s, sizeof s, "%d item%s", n,
			n == 1 ? "" : "s");
		if (m)
			l += snprintf(s + l, sizeof s - l, ", %d marked", m);
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

	XCopyArea(dpy, buf, win, gc, 0, 0, W, H, 0, 0);
}

static void resize_buffer(void)
{
	int was = sel_visible();
	if (xd)
		XftDrawDestroy(xd);
	if (buf)
		XFreePixmap(dpy, buf);
	buf = XCreatePixmap(dpy, win, W, H, depth);
	xd = XftDrawCreate(dpy, buf, vis, cmap);
	layout();
	if (was)
		show_sel();
	else
		clamp_top();
	dirty = 1;
}

/* ---- input method ------------------------------------------------------- */

/*
 * The IM named by XMODIFIERS (ibus, fcitx, ...) when it runs; otherwise
 * Xlib's built-in one ("@im=none", which still does compose and dead
 * keys) while we wait for the real one to appear.
 */
static int xim_local;

static void xim_instantiate(Display *d, XPointer client, XPointer call);

static void xim_close(void)
{
	if (xic)
		XDestroyIC(xic);
	if (xim)
		XCloseIM(xim);
	xic = NULL;
	xim = NULL;
}

static void xim_make_ic(void)
{
	static const XIMStyle want[] = {
		XIMPreeditNothing | XIMStatusNothing,
		XIMPreeditNothing | XIMStatusNone,
		XIMPreeditNone | XIMStatusNothing,
		XIMPreeditNone | XIMStatusNone,
	};
	XIMStyles *styles = NULL;
	XIMStyle style = 0;
	if (!XGetIMValues(xim, XNQueryInputStyle, &styles, NULL) && styles) {
		for (size_t w = 0; w < sizeof want / sizeof *want && !style; w++)
			for (int i = 0; i < styles->count_styles; i++)
				if (styles->supported_styles[i] == want[w]) {
					style = want[w];
					break;
				}
		XFree(styles);
	}
	if (style)
		xic = XCreateIC(xim, XNInputStyle, style, XNClientWindow, win,
			XNFocusWindow, win, NULL);
	long fev = 0;
	if (xic)
		XGetICValues(xic, XNFilterEvents, &fev, NULL);
	XSelectInput(dpy, win, EVENT_MASK | fev);
	if (xic && focused)
		XSetICFocus(xic);
}

static void xim_open_local(void)
{
	XSetLocaleModifiers("@im=none");
	if ((xim = XOpenIM(dpy, NULL, NULL, NULL))) {
		xim_local = 1;
		xim_make_ic();
	}
	XSetLocaleModifiers("");
}

static void xim_destroyed(XIM im, XPointer client, XPointer call)
{
	/* The IM server went away (e.g. ibus restarted); its ICs are gone
	 * too. Ignore news about an IM we no longer use. */
	if (im != xim)
		return;
	xim = NULL;
	xic = NULL;
	/* Registering runs xim_instantiate at once if the server is already
	 * back; only fall back to the built-in IM if it is not. */
	XRegisterIMInstantiateCallback(dpy, NULL, NULL, NULL,
		xim_instantiate, NULL);
	if (!xim)
		xim_open_local();
}

static void xim_instantiate(Display *d, XPointer client, XPointer call)
{
	if (xim && !xim_local)
		return;                 /* already on the real IM */
	XSetLocaleModifiers("");
	XIM im = XOpenIM(dpy, NULL, NULL, NULL);
	if (!im)
		return;
	XUnregisterIMInstantiateCallback(dpy, NULL, NULL, NULL,
		xim_instantiate, NULL);
	xim_close();
	xim = im;
	xim_local = 0;
	XIMCallback cb = { NULL, xim_destroyed };
	XSetIMValues(xim, XNDestroyCallback, &cb, NULL);
	xim_make_ic();
}

static void xim_init(void)
{
	xim_instantiate(dpy, NULL, NULL);
	if (xim)
		return;
	XRegisterIMInstantiateCallback(dpy, NULL, NULL, NULL,
		xim_instantiate, NULL);
	if (!xim)
		xim_open_local();
}

/* Text for a key without any IM, from the keysym rather than from
 * XLookupString, whose encoding depends on the locale. */
static int keysym_utf8(KeySym ks, char *out)
{
	uint32_t c;
	if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xa0 && ks <= 0xff))
		c = ks;
	else if ((ks & 0xff000000) == 0x01000000)
		c = ks & 0xffffff;
	else if (ks >= XK_KP_0 && ks <= XK_KP_9)
		c = '0' + (ks - XK_KP_0);
	else if (ks >= 0x20a0 && ks <= 0x20ac)  /* currency, e.g. EuroSign */
		c = ks;
	else if (ks == XK_KP_Decimal || ks == XK_KP_Add ||
		 ks == XK_KP_Subtract || ks == XK_KP_Multiply ||
		 ks == XK_KP_Divide || ks == XK_KP_Equal || ks == XK_KP_Space)
		c = ks == XK_KP_Space ? ' ' : (uint32_t)(ks - 0xff80);
	else
		return 0;
	if (c < 0x80) {
		out[0] = c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = 0xc0 | c >> 6;
		out[1] = 0x80 | (c & 0x3f);
		return 2;
	}
	if (c >= 0xd800 && c <= 0xdfff)
		return 0;
	if (c < 0x10000) {
		out[0] = 0xe0 | c >> 12;
		out[1] = 0x80 | ((c >> 6) & 0x3f);
		out[2] = 0x80 | (c & 0x3f);
		return 3;
	}
	if (c > 0x10ffff)
		return 0;
	out[0] = 0xf0 | c >> 18;
	out[1] = 0x80 | ((c >> 12) & 0x3f);
	out[2] = 0x80 | ((c >> 6) & 0x3f);
	out[3] = 0x80 | (c & 0x3f);
	return 4;
}

/* ---- window setup ------------------------------------------------------- */

/* A 32x32 folder for _NET_WM_ICON, which mwm shows on desktop icons. */
static void set_icon(void)
{
	enum { S = 32 };
	static unsigned long data[2 + S * S];
	data[0] = S;
	data[1] = S;
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			unsigned long p = 0;
			int tab = y >= 5 && y < 9 && x >= 3 && x < 15;
			int body = y >= 8 && y < 28 && x >= 3 && x < 29;
			if (tab || body)
				p = 0xffc9a85c;
			if (body && (y == 8 || y == 27 || x == 3 || x == 28))
				p = 0xff5f4f2a;
			if (tab && (y == 5 || x == 3 || x == 14))
				p = 0xff5f4f2a;
			data[2 + y * S + x] = p;
		}
	XChangeProperty(dpy, win, a_icon, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)data, 2 + S * S);
}

static void setup_window(void)
{
	XSetWindowAttributes wa = {
		.background_pixel = px_bg,
		.bit_gravity = NorthWestGravity,
		.event_mask = EVENT_MASK,
	};
	win = XCreateWindow(dpy, root, 0, 0, W, H, 0, depth, InputOutput,
		vis, CWBackPixel | CWBitGravity | CWEventMask, &wa);

	XSizeHints *sh = XAllocSizeHints();
	sh->flags = PMinSize;
	sh->min_width = 300;
	sh->min_height = 200;
	XWMHints *wh = XAllocWMHints();
	wh->flags = InputHint | StateHint;
	wh->input = True;
	wh->initial_state = NormalState;
	XClassHint ch = { "mfm", "Mfm" };
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sh, wh, &ch);
	XFree(sh);
	XFree(wh);

	a_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	a_ping = XInternAtom(dpy, "_NET_WM_PING", False);
	a_name = XInternAtom(dpy, "_NET_WM_NAME", False);
	a_icon_name = XInternAtom(dpy, "_NET_WM_ICON_NAME", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_icon = XInternAtom(dpy, "_NET_WM_ICON", False);
	a_pid = XInternAtom(dpy, "_NET_WM_PID", False);
	Atom prots[] = { a_delete, a_ping };
	XSetWMProtocols(dpy, win, prots, 2);
	long pid = getpid();
	XChangeProperty(dpy, win, a_pid, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *)&pid, 1);
	set_icon();

	gc = XCreateGC(dpy, win, 0, NULL);
	xim_init();
	resize_buffer();
}

/* ---- events ------------------------------------------------------------- */

static void key(KeySym ks, const char *txt, int n, unsigned state)
{
	if (IsModifierKey(ks))
		return;
	if (ks >= XK_A && ks <= XK_Z)
		ks += XK_a - XK_A;
	int ctrl = state & ControlMask, alt = state & Mod1Mask;
	int shift = state & ShiftMask;

	if (editing) {
		edit_key(ks, txt, n, ctrl);
		return;
	}
	if (prompt) {
		prompt_key(ks, txt, n, ctrl);
		return;
	}
	if (pend) {
		answer_delete(!ctrl && !alt && (ks == XK_y || ks == XK_j));
		return;
	}
	/* A type-ahead that timed out is over: Backspace means "parent". */
	if (search[0] && now_ms() - search_ms >= TYPEAHEAD_MS)
		search[0] = 0;
	int had_msg = status_msg[0] != 0;
	clear_status();
	if (ctrl) {
		switch (ks) {
		case XK_q: case XK_w: exit(0);
		case XK_h:
			show_hidden = !show_hidden;
			reload();
			set_status(0, "hidden files %s",
				show_hidden ? "shown" : "hidden");
			break;
		case XK_r: reload(); break;
		case XK_l: edit_begin(); break;
		case XK_t: open_terminal(); break;
		case XK_a: mark_all(); break;
		case XK_c: clip_set(0); break;
		case XK_x: clip_set(1); break;
		case XK_v: paste(); break;
		case XK_n:
			if (shift)
				prompt_begin(PR_MKDIR);
			break;
		}
		return;
	}
	if (alt) {
		if (ks == XK_Up || ks == XK_KP_Up)
			go_parent();
		else if (ks == XK_Home || ks == XK_KP_Home)
			go_home();
		else if (ks == XK_F4)
			exit(0);
		return;
	}
	int vis = rows_vis();
	switch (ks) {
	case XK_Up: case XK_KP_Up: move_sel(-1); return;
	case XK_Down: case XK_KP_Down: move_sel(1); return;
	case XK_Prior: case XK_KP_Prior: move_sel(-vis); return;
	case XK_Next: case XK_KP_Next: move_sel(vis); return;
	case XK_Home: case XK_KP_Home: move_sel(-nents); return;
	case XK_End: case XK_KP_End: move_sel(nents); return;
	case XK_Return: case XK_KP_Enter: activate(sel); return;
	case XK_Right: case XK_KP_Right:
		if (sel < nents && ents[sel].dir)
			activate(sel);
		return;
	case XK_Left: case XK_KP_Left: go_parent(); return;
	case XK_BackSpace:
		if (search[0]) {
			search[u8_prev(search, strlen(search))] = 0;
			search_ms = now_ms();
			dirty = 1;
		} else {
			go_parent();
		}
		return;
	case XK_Escape:         /* one thing at a time */
		if (search[0])
			search[0] = 0;
		else if (!had_msg)
			unmark_all();
		dirty = 1;
		return;
	case XK_F5: reload(); return;
	case XK_F2: prompt_begin(PR_RENAME); return;
	case XK_F7: prompt_begin(PR_MKDIR); return;
	case XK_Insert: case XK_KP_Insert:
		toggle_mark(sel);
		move_sel(1);
		return;
	case XK_Delete: case XK_KP_Delete:
		if (shift)
			ask_delete();
		else
			trash();
		return;
	case XK_space:
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

static void on_key(XKeyEvent *ev)
{
	char sbuf[64], *txt = sbuf, *heap = NULL;
	KeySym ks = NoSymbol;
	int n = 0;
	if (xic) {
		Status st;
		n = Xutf8LookupString(xic, ev, txt, sizeof sbuf - 1, &ks, &st);
		if (st == XBufferOverflow && (heap = malloc(n + 1))) {
			txt = heap;
			n = Xutf8LookupString(xic, ev, txt, n, &ks, &st);
		}
		if (st != XLookupKeySym && st != XLookupBoth)
			ks = NoSymbol;
		if (st != XLookupChars && st != XLookupBoth)
			n = 0;
	} else {
		char ignored[32];
		XLookupString(ev, ignored, sizeof ignored, &ks, NULL);
		n = keysym_utf8(ks, txt);
	}
	key(ks, txt, n, ev->state);
	free(heap);
}

static void on_button(XButtonEvent *ev)
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
	case Button4: top -= 3; clamp_top(); dirty = 1; return;
	case Button5: top += 3; clamp_top(); dirty = 1; return;
	case 8: go_parent(); return;
	case Button1: break;
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
		int i = top + (y - (r_list.y + BEV + 2)) / rowh;
		if (y < r_list.y + BEV + 2 || i >= nents ||
		    i >= top + rows_vis())
			return;
		search[0] = 0;
		clear_status();
		if (ev->state & ControlMask) {
			toggle_mark(i);
			sel = i;
			last_idx = -1;
		} else if (ev->state & ShiftMask) {
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
		    ev->time - last_click < DCLICK_MS) {
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

static void on_motion(XMotionEvent *ev)
{
	if (!dragging)
		return;
	int ty, th, sy, sh, vis = rows_vis();
	sb_geom(&ty, &th, &sy, &sh);
	if (th > sh && nents > vis) {
		top = (int)((long)(ev->y - drag_off - ty) * (nents - vis) /
			(th - sh));
		clamp_top();
		dirty = 1;
	}
}

static void on_release(XButtonEvent *ev)
{
	if (ev->button != Button1)
		return;
	dragging = 0;
	if (up_pressed) {
		up_pressed = 0;
		dirty = 1;
		if (in(r_up, ev->x, ev->y))
			go_parent();
	}
}

static void handle(XEvent *ev)
{
	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0)
			dirty = 1;
		break;
	case ConfigureNotify:
		if (ev->xconfigure.width != W || ev->xconfigure.height != H) {
			W = ev->xconfigure.width;
			H = ev->xconfigure.height;
			resize_buffer();
		}
		break;
	case KeyPress: on_key(&ev->xkey); break;
	case ButtonPress: on_button(&ev->xbutton); break;
	case ButtonRelease: on_release(&ev->xbutton); break;
	case MotionNotify:
		while (XCheckTypedWindowEvent(dpy, win, MotionNotify, ev))
			;
		on_motion(&ev->xmotion);
		break;
	case FocusIn:
		focused = 1;
		if (xic)
			XSetICFocus(xic);
		break;
	case FocusOut:
		focused = 0;
		if (xic)
			XUnsetICFocus(xic);
		break;
	case MappingNotify: XRefreshKeyboardMapping(&ev->xmapping); break;
	case ClientMessage:
		if (ev->xclient.message_type != a_protocols)
			break;
		if ((Atom)ev->xclient.data.l[0] == a_delete)
			exit(0);
		if ((Atom)ev->xclient.data.l[0] == a_ping) {
			ev->xclient.window = root;
			XSendEvent(dpy, root, False, SubstructureNotifyMask |
				SubstructureRedirectMask, ev);
		}
		break;
	}
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

int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && (!strcmp(argv[1], "-h") ||
	    !strcmp(argv[1], "--help")))) {
		fprintf(stderr, "usage: mfm [directory or file]\n"
			"Environment: MFM_OPEN (default xdg-open), "
			"MFM_TERMINAL / MWM_TERMINAL,\n"
			"MFM_FONT (default \"Sans:size=10\"), "
			"MWM_INACTIVE, MWM_ACTIVE.\n");
		return argc == 2 ? 0 : 1;
	}
	setlocale(LC_ALL, "");
	/* A locale glibc has but Xlib does not know leaves us without any
	 * input method; C.UTF-8 for LC_CTYPE alone (collation and dates
	 * stay) gives Xlib a locale it supports. */
	if (!XSupportsLocale() && !setlocale(LC_CTYPE, "C.UTF-8"))
		setlocale(LC_CTYPE, "C");
	if (XSupportsLocale())
		XSetLocaleModifiers("");
	signal(SIGCHLD, SIG_IGN);

	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "mfm: cannot open display\n");
		return 1;
	}
	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	depth = DefaultDepth(dpy, scr);
	theme_init();
	layout();
	setup_window();
	start(argc == 2 ? argv[1] : NULL);
	set_title();
	XMapWindow(dpy, win);

	int xfd = ConnectionNumber(dpy);
	uint64_t next_poll = now_ms() + POLL_MS;
	for (;;) {
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
		if (dirty && !XPending(dpy)) {
			draw();
			dirty = 0;
		}
		if (!XPending(dpy)) {
			uint64_t wake = next_poll;
			if (search[0])
				wake = MIN(wake, search_ms + TYPEAHEAD_MS);
			now = now_ms();
			uint64_t ms = wake > now ? wake - now : 0;
			struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
			fd_set fds;
			XFlush(dpy);
			FD_ZERO(&fds);
			FD_SET(xfd, &fds);
			if (job.fd >= 0)
				FD_SET(job.fd, &fds);
			select(MAX(xfd, job.fd) + 1, &fds, NULL, NULL, &tv);
			continue;
		}
		XEvent ev;
		XNextEvent(dpy, &ev);
		if (XFilterEvent(&ev, None))
			continue;
		handle(&ev);
	}
}
