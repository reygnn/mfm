/*
 * mtk.h - the Motif toolkit of mwc's clients: a Wayland window in the look of mwm,
 * drawn with Cairo and Pango.
 *
 * Colours, the mwmrc, drawing, key handling and a line to edit need no compositor
 * (color.c, rc.c, draw.c, input.c, field.c); the Wayland side is wl.c. Every name
 * starts with mtk_ or MTK_. Colours are 0xrrggbb.
 */
#ifndef MTK_H
#define MTK_H

#include <cairo.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#define MTK_VERSION "0.8.0"

/* ---- colours (color.c) ---------------------------------------------------- */

/* rgb moved towards to by f, per channel, rounded. */
uint32_t mtk_mix(uint32_t rgb, uint32_t to, float f);

/* A face and the colours drawn on it. */
struct mtk_shade {
	uint32_t bg, top, bot, fg;
};

/* top and bot of bg as Motif 2.3.8's CalculateColorsRGB() gives them, cut to 8
 * bits as the screen shows them; fg as given. The same as mwc's shade_from(). */
struct mtk_shade mtk_shade(uint32_t bg, uint32_t fg);

/* Black or white text on bg, as Motif picks it and mwc does for its titles:
 * black above 70% of Motif's brightness. */
uint32_t mtk_text_color(uint32_t bg);

/* WCAG relative luminance, and the contrast ratio of a and b, 1 to 21. */
float mtk_luminance(uint32_t c);
float mtk_contrast_ratio(uint32_t a, uint32_t b);

/* Black or white, whichever has the higher WCAG contrast on bg: at least 4.5:1 on
 * any bg, which mtk_text_color() is not. */
uint32_t mtk_best_contrast(uint32_t bg);

/* bg moved towards fg as far as WCAG AA's 4.5:1 for small text needs, at least
 * 12/20 of the way; fg itself if even that falls short. The dock tools' labels. */
uint32_t mtk_dimmed(uint32_t bg, uint32_t fg);

/* c as it is if it has 4.5:1 on bg, else moved towards to just as far as that
 * needs; to itself if even that falls short. */
uint32_t mtk_readable(uint32_t bg, uint32_t c, uint32_t to);

/* What mwc reads: "#rrggbb", names like "gold", and rgb:r/g/b with 1 to 4 hex
 * digits per channel. 0 for NULL, "" and anything else; *out is then untouched. */
int mtk_parse_color(const char *s, uint32_t *out);

/* ---- the mwmrc and the theme (rc.c) ---------------------------------------- */

/* The value of the last global "Mwm*key: value" line of $XDG_CONFIG_HOME/mwc/mwmrc,
 * else ~/.config/mwc/mwmrc, in val[n]; NULL if there is none or it is too long.
 * Key and "Mwm" in any case, "*" or ".". */
char *mtk_mwmrc_get(const char *key, char *val, size_t n);

/* The value of the last "Mwm*app*key: value" line, for one client. */
char *mtk_mwmrc_client_get(const char *app, const char *key, char *val, size_t n);

/* The colours mwc draws with: MWC_INACTIVE, else Mwm*background, else #aaaaaa;
 * MWC_ACTIVE, else Mwm*activeBackground, else #c6c1aa, IRIX's Base scheme. The
 * text is Mwm*foreground and Mwm*activeForeground, else mtk_text_color(); on the
 * built-in grey black, as mwc has it. */
struct mtk_theme {
	struct mtk_shade inactive, active;
	int has_fg, has_active_fg;      /* the user set the text colour */
	uint32_t dim;                   /* mtk_dimmed() on inactive */
};

void mtk_theme_load(struct mtk_theme *t);

/* ---- drawing (draw.c) ------------------------------------------------------ */

#define MTK_BEVEL 2                     /* mwm's shadow */
#define MTK_BORDER (MTK_BEVEL + 1)      /* mwm's thin border, as mwc's dock has it */

void mtk_set_rgb(cairo_t *cr, uint32_t c);
void mtk_fill(cairo_t *cr, uint32_t c, int x, int y, int w, int h);

/* A bevel t pixels wide on the edge of x, y, w, h: tl along the top and left, br
 * along the bottom and right. The top right and bottom left corners are split on
 * the diagonal, as Motif's shadows; the pixel on it is tl's. Raised: tl is the
 * light shadow, sunken: the dark one. */
void mtk_bevel(cairo_t *cr, int x, int y, int w, int h, int t, uint32_t tl,
	uint32_t br);

/* The same with s's shadows: raised, or sunken if in. */
void mtk_shadow(cairo_t *cr, int x, int y, int w, int h, int t,
	const struct mtk_shade *s, int in);

/* The rows of a buffer that differ from the one shown before it, both h rows of
 * stride bytes, as at most max runs in rows[i][0] (first row) and rows[i][1] (how
 * many): a gap of up to gap rows is not worth a run of its own, and the last run
 * takes in the rest. 0 if nothing changed. For wl_surface.damage_buffer, so the
 * compositor copies and composites only those. */
int mtk_damage_rows(const void *now, const void *was, size_t stride, int h, int gap,
	int max, int rows[][2]);

/* A groove of two lines, dark then light: across from y, down from x. */
void mtk_groove_h(cairo_t *cr, int x, int y, int w, const struct mtk_shade *s);
void mtk_groove_v(cairo_t *cr, int x, int y, int h, const struct mtk_shade *s);

/* mwm's thin border around a window of w x h: MTK_BORDER pixels of s's face with
 * a raised bevel. The inside is left as it is. */
void mtk_border(cairo_t *cr, int w, int h, const struct mtk_shade *s);

/* The font of mwc's dock, DejaVu Sans 10, fixed so that the tools in it are as
 * high as each other. A warning on stderr, after name, if fontconfig gives
 * another. Free it with pango_font_description_free(). */
PangoFontDescription *mtk_dock_font(const char *name);

/* The font named, or NULL with a warning if Pango would take some other family
 * for it. */
PangoFontDescription *mtk_font(const char *name, const char *pango_name);

/* A single-line layout in that font, on Cairo's default font map. */
PangoLayout *mtk_layout_new(const PangoFontDescription *font);

/* s in l, and its width in pixels. */
int mtk_text_width(PangoLayout *l, const char *s);

/* s in l, in colour c, its line's top left at x, y. */
void mtk_text(cairo_t *cr, PangoLayout *l, const char *s, uint32_t c, int x, int y);

/* ---- keys and cursors (input.c) ------------------------------------------ */

#define MTK_MOD_SHIFT 1u
#define MTK_MOD_CTRL 2u
#define MTK_MOD_ALT 4u
#define MTK_MOD_LOGO 8u

/* XCURSOR_SIZE as given in s: 24 if NULL or bad, at most 256. */
int mtk_cursor_size(const char *s);

/* A theme has only some sizes: Adwaita gives 30 for 28. A buffer scale that
 * doesn't divide the size is a protocol error. A w x h image loaded for scale s
 * is shown at *buf_scale; with a viewport (has_vp) at *vw x *vh, its size over
 * the scale, else they are -1 and the buffer scale is the largest up to s that
 * divides it, and the cursor that much larger. */
void mtk_cursor_fit(int w, int h, int s, int has_vp, int *buf_scale, int *vw,
	int *vh);

/* The buffer scale for the compositor's preferred factor: at most 16; 0 for a
 * factor below 1, which is to be ignored. */
int mtk_buffer_scale(int factor);

/* Cairo draws at most this many pixels wide or high; a larger buffer stays black. */
#define MTK_CAIRO_MAX 32767

/* The scale to draw a window of w x h surface pixels at, for the buffer scale s:
 * the largest up to s whose buffer Cairo can draw and a wl_shm pool, below 2 GiB,
 * can hold, so that a huge window is drawn at a lower scale and enlarged by the
 * compositor. At least 1. */
int mtk_draw_scale(int w, int h, int s);

/* The seats the compositor has announced and not removed, oldest first: one is
 * bound, the others wait for it to go. */
#define MTK_MAX_SEATS 8
struct mtk_seats {
	unsigned name[MTK_MAX_SEATS], ver[MTK_MAX_SEATS];
	int n;
};

/* A seat announced; 0 if the list is full and it is not kept. */
int mtk_seats_add(struct mtk_seats *s, unsigned name, unsigned ver);
/* A global removed: dropped if it is a seat in the list. */
void mtk_seats_remove(struct mtk_seats *s, unsigned name);
/* The seat to take, the oldest left; 0 if there is none. */
int mtk_seats_next(const struct mtk_seats *s, unsigned *name, unsigned *ver);

/* Whether the key quits a small tool: Esc, q or Q, with Ctrl also w and W. With
 * Alt or Super held no key does. */
int mtk_key_quits(xkb_keysym_t ks, unsigned mods);

/* The modifiers in effect, as MTK_MOD_*. */
unsigned mtk_mods(struct xkb_state *st);

/* The locale's compose table, by LC_ALL, LC_CTYPE, LANG, else C; NULL if there is
 * none. */
struct xkb_compose_state *mtk_compose_new(struct xkb_context *ctx);

/* A key press or repeat, xkb keycode kc, through compose (which may be NULL):
 * the keysym and the text it makes, in utf8[64], NUL-terminated. 0 if the key
 * only went into a compose sequence, or cancelled one with a character, and does
 * nothing else. */
int mtk_key_translate(struct xkb_state *st, struct xkb_compose_state *compose,
	uint32_t kc, xkb_keysym_t *sym, char utf8[64]);

/* For a shortcut, the key as it is in the first layout of map that gives a Latin
 * letter or a digit there, as Qt and mfm read it: in Russian, Ctrl+U is the key that
 * is U in a Latin layout. A digit counts on its level with Shift too: AZERTY has "&"
 * there without it, and Ctrl+1 would never come. Else ks, or for a key past Latin-1
 * the first printable ASCII any layout has on it. */
xkb_keysym_t mtk_latin_sym(struct xkb_keymap *map, uint32_t kc, xkb_keysym_t ks);

/* Key repeat, by the compositor's rate (keys a second, 0 for none) and delay
 * (ms). Times are in ms on any clock that does not go back. */
struct mtk_repeat {
	uint32_t kc;                    /* the repeating key, 0 for none */
	uint64_t next;                  /* when it repeats next */
	int rate, delay;
};

void mtk_repeat_init(struct mtk_repeat *r);
void mtk_repeat_info(struct mtk_repeat *r, int rate, int delay);
/* kc pressed: it repeats if it may (by the keymap and the client). Another key
 * that repeats stops the one before; a modifier does not. */
void mtk_repeat_press(struct mtk_repeat *r, uint32_t kc, int keymap_repeats,
	int may_repeat, uint64_t now);
void mtk_repeat_release(struct mtk_repeat *r, uint32_t kc);
void mtk_repeat_stop(struct mtk_repeat *r);
/* The key that repeats now, or 0; the next repeat is set from now. */
uint32_t mtk_repeat_due(struct mtk_repeat *r, uint64_t now);
/* ms until the next repeat, at most INT_MAX; -1 if none waits. */
int mtk_repeat_timeout(const struct mtk_repeat *r, uint64_t now);

/* Scrolling on one axis, in steps: a wheel's own, or a touchpad's distance, of which
 * MTK_SCROLL_STEP pixels make one. At most MTK_SCROLL_MAX steps a frame. */
#define MTK_SCROLL_STEP 15
#define MTK_SCROLL_MAX 20
struct mtk_scroll {
	double acc;             /* distance not yet a step */
	int steps;              /* the wheel's, this frame */
};

void mtk_scroll_value(struct mtk_scroll *s, double v);
void mtk_scroll_discrete(struct mtk_scroll *s, int steps);
/* The finger has left the touchpad: what is left of the distance goes. */
void mtk_scroll_stop(struct mtk_scroll *s);
/* The frame is over: its steps. A wheel's count and drop the distance; else whole
 * steps of the distance are taken from it. */
int mtk_scroll_frame(struct mtk_scroll *s);

/* ---- a line to edit (field.c) --------------------------------------------- */

/* The cursor position before (dir < 0) or after at in s: a whole grapheme, a flag
 * or a letter with its marks, as Pango tells them apart. Next to a byte that is not
 * UTF-8 it goes by a letter and the combining marks after it. */
size_t mtk_grapheme_step(const char *s, size_t at, int dir);

/* A line of text in the client's buffer text[size]. Fresh: nothing done since
 * mtk_field_set(); with selects the text before the cursor is then selected, and
 * typing or deleting replaces it. accept, if set, sees what an edit would make of
 * the text, in a buffer of size bytes: it may change it, or refuse it with 0. */
struct mtk_field {
	char *text;
	size_t size;
	size_t cur;                     /* in bytes, on a character boundary */
	int fresh, selects;
	int (*accept)(char *text, size_t size, void *data);
	void *data;
};

enum {
	MTK_FIELD_NONE,                 /* nothing to show */
	MTK_FIELD_MOVED,                /* the cursor, or the selection gone */
	MTK_FIELD_CHANGED,              /* the text */
	MTK_FIELD_ENTER,                /* Return */
	MTK_FIELD_CANCEL,               /* Esc */
};

/* s in the field, cut before a character if it is too long, the cursor at its end,
 * fresh. */
void mtk_field_set(struct mtk_field *f, const char *s);

/* A key: ks as for a shortcut (mtk_key.latin) when Ctrl is held, its text and
 * the modifiers. Left, Right, Home, End, BackSpace, Delete, Ctrl+U and typing;
 * Return and Esc only say so. */
int mtk_field_key(struct mtk_field *f, xkb_keysym_t ks, const char *utf8,
	unsigned mods);

/* text, valid UTF-8, in l on one line in x, y, w, h: scrolled so the cursor at
 * byte cur shows, shortened with "..." where it doesn't fit, as mfm draws its
 * fields. With selected the text before the cursor is drawn selected, sel_fg on
 * sel_bg. */
void mtk_field_draw(cairo_t *cr, PangoLayout *l, const char *text, size_t cur,
	int selected, int x, int y, int w, int h, uint32_t fg, uint32_t sel_bg,
	uint32_t sel_fg);

/* ---- Wayland (wl.c) -------------------------------------------------------- */

struct mtk;
struct mtk_window;
struct wl_display;
struct wl_registry;
struct wl_seat;
struct wl_surface;
struct xdg_toplevel;

/* What the client wants of the connection. All but name may be left 0. */
struct mtk_config {
	const char *name;       /* in messages, and the default app_id */
	/* The globals mtk doesn't bind itself, for the client to bind. */
	void (*global)(struct mtk *m, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t ver, void *data);
	void (*global_remove)(struct mtk *m, uint32_t name, void *data);
	/* Each time before the loop waits, after what was due is drawn. */
	void (*idle)(struct mtk *m, void *data);
	void *data;
};

/* The connection, the seat and the event loop. NULL with a message on stderr if
 * there is no compositor or it lacks wl_compositor 4, wl_shm or xdg_wm_base.
 * A lost connection later prints why and exits with 1. */
struct mtk *mtk_open(const struct mtk_config *cfg);
void mtk_close(struct mtk *m);

struct wl_display *mtk_display(struct mtk *m);
struct wl_seat *mtk_seat(struct mtk *m);        /* NULL while there is none */
/* The serial of the last input event, for grabs and the clipboard. */
uint32_t mtk_serial(struct mtk *m);
/* Milliseconds on CLOCK_MONOTONIC. */
uint64_t mtk_now(void);

/* The loop: fd callbacks and one-shot timers, run until mtk_quit(). */
typedef void (*mtk_fd_fn)(struct mtk *m, int fd, short revents, void *data);
typedef void (*mtk_timer_fn)(struct mtk *m, void *data);

/* An fd watched for events; 0, or -1 if there are too many. */
int mtk_watch_fd(struct mtk *m, int fd, short events, mtk_fd_fn fn, void *data);
void mtk_unwatch_fd(struct mtk *m, int fd);
/* fn once, ms from now; an id above 0 for mtk_timer_cancel(), 0 if there are too
 * many. */
unsigned mtk_timer(struct mtk *m, int ms, mtk_timer_fn fn, void *data);
void mtk_timer_cancel(struct mtk *m, unsigned id);

/* Runs until mtk_quit(); returns its status. */
int mtk_run(struct mtk *m);
void mtk_quit(struct mtk *m, int status);

/* For a client with a loop of its own (libpulse's): before its poll, the Wayland
 * fd to watch and the timeout lowered to mtk's next timer; after it, that fd's
 * revents. mtk_after_poll() runs the due timers but not the fds of
 * mtk_watch_fd(). */
void mtk_before_poll(struct mtk *m, struct pollfd *pfd, int *timeout);
void mtk_after_poll(struct mtk *m, short revents);

/* A key, after the keymap and compose. */
struct mtk_key {
	uint32_t kc;            /* xkb keycode */
	xkb_keysym_t sym;
	xkb_keysym_t latin;     /* sym for a shortcut: mtk_latin_sym() */
	const char *utf8;       /* the text it makes, "" for none */
	unsigned mods;          /* MTK_MOD_* */
	int repeat;             /* not pressed but repeated */
};

#define MTK_BUTTON_LEFT 1u
#define MTK_BUTTON_MIDDLE 2u
#define MTK_BUTTON_RIGHT 4u

enum mtk_pointer_kind {
	MTK_ENTER, MTK_LEAVE, MTK_MOTION, MTK_PRESS, MTK_RELEASE, MTK_SCROLL,
};

/* A pointer event, in surface pixels. Scrolling comes at the end of a frame, in
 * steps as mtk_scroll_frame() makes them, and in pixels (dx, dy); down and right
 * are positive. */
struct mtk_pointer {
	enum mtk_pointer_kind kind;
	double x, y;
	uint32_t button;        /* BTN_LEFT and so on */
	uint32_t serial, time;
	unsigned mods;
	unsigned buttons;       /* MTK_BUTTON_* held before this event */
	int steps_x, steps_y;
	double dx, dy;
};

#define MTK_STATE_ACTIVATED 1u
#define MTK_STATE_MAXIMIZED 2u
#define MTK_STATE_FULLSCREEN 4u
#define MTK_STATE_TILED 8u

/* No window side is larger, whatever a compositor asks. */
#define MTK_MAX_SIZE 8192

/* A resizable window's size after a configure of cw x ch in states, into *w x *h
 * (input.c). 0 leaves it to the client: its free size *fw x *fh, which it had last
 * while not maximized, fullscreen or tiled, and which a free configure sets. Never
 * below the minimum, which a tiling compositor may ignore, nor above
 * MTK_MAX_SIZE. */
void mtk_configure_size(int cw, int ch, unsigned states, int min_w, int min_h,
	int *fw, int *fh, int *w, int *h);

struct mtk_window_listener {
	/* Draw w x h surface pixels on cr, which the scale is already applied to,
	 * the antialias off (Pango's text keeps its own). Required. */
	void (*paint)(struct mtk_window *w, cairo_t *cr, int width, int height,
		void *data);
	/* The compositor's size, w and h 0 to leave it to the client, and the
	 * MTK_STATE_* flags. Called before the paint it causes. */
	void (*configure)(struct mtk_window *w, int width, int height,
		unsigned states, void *data);
	/* The close button. NULL: mtk_quit(m, 0). */
	void (*close)(struct mtk_window *w, void *data);
	void (*focus)(struct mtk_window *w, int in, void *data);
	void (*key)(struct mtk_window *w, const struct mtk_key *k, void *data);
	/* Whether a held key repeats; NULL: every key the keymap repeats. */
	int (*key_repeats)(struct mtk_window *w, const struct mtk_key *k,
		void *data);
	void (*pointer)(struct mtk_window *w, const struct mtk_pointer *p,
		void *data);
	/* The buffer scale changed; a paint follows. */
	void (*scale)(struct mtk_window *w, int scale, void *data);
};

struct mtk_window_config {
	const char *app_id;     /* NULL: the mtk_config's name */
	const char *title;      /* NULL: the app_id */
	int width, height;      /* the size until the compositor sets one */
	int fixed;              /* always that size: min and max size alike */
	int min_width, min_height;
	int server_decoration;  /* ask for mwc's frame with xdg-decoration */
	int maximized;          /* ask for it before the first commit */
};

struct mtk_window *mtk_window_new(struct mtk *m, const struct mtk_window_config *c,
	const struct mtk_window_listener *l, void *data);
/* Also the popups it has. */
void mtk_window_destroy(struct mtk_window *w);

/* A popup of parent, as a menu: width x height, below and right of x, y in the
 * parent's surface pixels, else flipped or slid to stay on the screen. With grab,
 * the serial of the press that opens it (mtk_pointer.serial), it takes the
 * keyboard and the pointer while it is up: the keys go to it whichever window the
 * compositor gives them, and the compositor still sends what happens over the
 * client's other windows to them. The compositor may end it, as on a click
 * on another client: then close, else it is destroyed. */
struct mtk_popup_config {
	int x, y, width, height;
	int grab;
	uint32_t serial;
};

struct mtk_window *mtk_popup_new(struct mtk_window *parent,
	const struct mtk_popup_config *c, const struct mtk_window_listener *l,
	void *data);
/* Where the compositor put it, in its parent's surface pixels. */
void mtk_popup_position(const struct mtk_window *popup, int *x, int *y);

/* Paint again, at the next frame. */
void mtk_window_redraw(struct mtk_window *w);
/* The client's own size; for a fixed window also its min and max size. While not
 * maximized, fullscreen or tiled it is the free size too. */
void mtk_window_set_size(struct mtk_window *w, int width, int height);
void mtk_window_set_min_size(struct mtk_window *w, int width, int height);
void mtk_window_set_title(struct mtk_window *w, const char *title);
/* The cursor on the window, by its name in the theme ("default", "text",
 * "pointer"...); NULL for the default. */
void mtk_window_set_cursor(struct mtk_window *w, const char *name);

void mtk_window_size(const struct mtk_window *w, int *width, int *height);
int mtk_window_scale(const struct mtk_window *w);
int mtk_window_focused(const struct mtk_window *w);
struct mtk *mtk_window_mtk(const struct mtk_window *w);
struct wl_surface *mtk_window_surface(const struct mtk_window *w);
/* NULL for a popup. */
struct xdg_toplevel *mtk_window_toplevel(const struct mtk_window *w);
/* For tests: how often it has painted, and whether a paint is due. */
unsigned mtk_window_paints(const struct mtk_window *w);
int mtk_window_dirty(const struct mtk_window *w);
/* For tests: how often a cursor theme was loaded. */
unsigned mtk_cursor_loads(struct mtk *m);

#endif
