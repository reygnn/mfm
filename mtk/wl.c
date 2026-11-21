/*
 * wl.c - the Wayland side of mtk: the connection, the seat, windows on wl_shm
 * buffers, the cursor and the event loop. What it decides without Wayland is in
 * input.c, so that make check can test it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

#include "mtk.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#ifndef MIN            /* glib has it too */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#define MAX_WATCH 16
#define MAX_TIMERS 32

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
	struct mtk *m;
	struct shmbuf *next_stale;      /* in m->stale until then */
};

struct mtk_window {
	struct mtk *m;
	struct mtk_window *next;
	const struct mtk_window_listener *l;
	void *data;
	struct wl_surface *surface;
	struct xdg_surface *xdg;
	struct xdg_toplevel *toplevel;  /* or, for a popup: */
	struct xdg_popup *popup;
	struct mtk_window *parent;
	int pop_x, pop_y;               /* where the compositor put it */
	int grab;                       /* the popup took the seat's grab */
	struct zxdg_toplevel_decoration_v1 *deco;
	struct shmbuf *bufs[2];
	struct shmbuf *shown;           /* the last committed, to damage only changes */
	int shown_scale;
	struct wl_callback *frame;      /* a frame is on its way */
	int configured;                 /* the first configure has arrived */
	int need_commit;                /* a configure waits for its commit */
	int dirty;
	int w, h, fixed;
	int min_w, min_h, sized_w, sized_h;     /* sized_: as told the compositor */
	int cfg_w, cfg_h;               /* the toplevel configure's, until acked */
	unsigned cfg_states;
	unsigned states;                /* the last acked */
	int free_w, free_h;             /* the size while not maximized or tiled */
	int scale;                      /* buffer pixels per surface pixel */
	int focused;
	unsigned paints;
	char *cursor;                   /* its name in the theme, NULL: default */
};

struct watch {
	int fd;
	short events;
	mtk_fd_fn fn;
	void *data;
};

struct timer {
	unsigned id;            /* 0: free */
	uint64_t at;
	mtk_timer_fn fn;
	void *data;
};

struct mtk {
	struct mtk_config cfg;
	struct wl_display *dpy;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wp_viewporter *viewporter;
	struct zxdg_decoration_manager_v1 *deco_mgr;
	struct mtk_window *windows;
	struct shmbuf *stale;           /* no window's, still the compositor's */

	struct wl_seat *seat;
	uint32_t seat_global;           /* its name in the registry */
	struct mtk_seats seats;         /* all announced, for when it goes */
	uint32_t serial;

	struct wl_keyboard *keyboard;
	struct xkb_context *xkb;
	struct xkb_keymap *kb_map;
	struct xkb_state *kb_state;
	struct xkb_compose_state *compose;
	struct mtk_repeat repeat;
	struct mtk_window *kb_win;

	struct wl_pointer *pointer;
	struct mtk_window *ptr_win;
	uint32_t ptr_serial;            /* the last enter's, for set_cursor */
	double ptr_x, ptr_y;
	unsigned buttons;               /* MTK_BUTTON_*, held */
	int axis;                       /* scrolling waits for the frame */
	struct mtk_scroll scroll_x, scroll_y;
	double dx, dy;
	uint32_t axis_time;

	struct wl_surface *cursor_surf;
	struct wp_viewport *cursor_vp;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor_image *cursor_img;
	int cursor_scale;               /* the scale the theme was loaded for */
	int cursor_buf_scale;           /* the buffer scale its image is shown at */
	int cursor_vw, cursor_vh;       /* its size on the window, if the viewport sets it */
	char cursor_name[32];           /* the one loaded */
	unsigned cursor_loads;

	struct watch watch[MAX_WATCH];
	int nwatch;
	struct timer timers[MAX_TIMERS];
	unsigned timer_id;
	int reading;                    /* between prepare_read and read_events */
	int quit, status;
};

static void lost_connection(struct mtk *m)
{
	fprintf(stderr, "%s: lost the connection to the compositor: %s\n", m->cfg.name,
		strerror(wl_display_get_error(m->dpy)));
	exit(1);
}

uint64_t mtk_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- buffers ------------------------------------------------------------ */

static void buf_free(struct shmbuf *b)
{
	if (b->stale)
		for (struct shmbuf **p = &b->m->stale; *p; p = &(*p)->next_stale)
			if (*p == b) {
				*p = b->next_stale;
				break;
			}
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

static struct shmbuf *buf_new(struct mtk *m, int w, int h)
{
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
	size_t size = (size_t)stride * h;
	struct shmbuf *b = calloc(1, sizeof *b);
	int fd = memfd_create(m->cfg.name, MFD_CLOEXEC);
	void *data = MAP_FAILED;
	if (b && fd >= 0 && ftruncate(fd, size) == 0)
		data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "%s: no memory for a %dx%d buffer: %s\n", m->cfg.name, w,
			h, strerror(errno));
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(m->shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	b->m = m;
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

/* A buffer no window draws on any more: freed now, or once the compositor lets go
 * of it, or by mtk_close(). */
static void buf_drop(struct shmbuf *b)
{
	if (!b->busy) {
		buf_free(b);
		return;
	}
	b->stale = 1;
	b->next_stale = b->m->stale;
	b->m->stale = b;
}

/* A buffer of this size to draw on, or NULL while the compositor holds both; its
 * release wakes the loop. */
static struct shmbuf *get_buffer(struct mtk_window *win, int w, int h)
{
	for (int i = 0; i < 2; i++) {
		struct shmbuf *b = win->bufs[i];
		if (!b || (b->w == w && b->h == h))
			continue;
		if (win->shown == b)
			win->shown = NULL;
		buf_drop(b);
		win->bufs[i] = NULL;
	}
	/* Not the one shown, if the other is free: that keeps what the compositor has,
	 * to compare the next picture with. */
	for (int i = 0; i < 2; i++)
		if (win->bufs[i] && !win->bufs[i]->busy && win->bufs[i] != win->shown)
			return win->bufs[i];
	for (int i = 0; i < 2; i++)
		if (!win->bufs[i])
			return win->bufs[i] = buf_new(win->m, w, h);
	for (int i = 0; i < 2; i++)
		if (win->bufs[i] && !win->bufs[i]->busy)
			return win->bufs[i];
	return NULL;
}

/* b committed, with damage for the rows that differ from what was shown: the
 * compositor copies and composites only those. */
static void damage(struct mtk_window *win, struct shmbuf *b, int s)
{
	enum { RECTS = 4, GAP = 8 };    /* a gap of a few rows is not worth a rect */
	const struct shmbuf *o = win->shown;
	if (!o || o == b || o->w != b->w || o->h != b->h || win->shown_scale != s) {
		wl_surface_damage_buffer(win->surface, 0, 0, b->w, b->h);
		return;
	}
	int rows[RECTS][2];
	int n = mtk_damage_rows(b->data, o->data, b->size / b->h, b->h, GAP * s, RECTS,
		rows);
	for (int i = 0; i < n; i++)
		wl_surface_damage_buffer(win->surface, 0, rows[i][0], b->w, rows[i][1]);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	struct mtk_window *win = data;
	wl_callback_destroy(cb);
	win->frame = NULL;
}

static const struct wl_callback_listener frame_listener = { frame_done };

/* 0 if the compositor holds both buffers: try again after a release. */
static int draw(struct mtk_window *win)
{
	int s = mtk_draw_scale(win->w, win->h, win->scale);
	/* Past what even scale 1 allows the buffer is cut: no output is that large. */
	int bw = win->w * s > MTK_CAIRO_MAX ? MTK_CAIRO_MAX : win->w * s;
	int bh = win->h * s > MTK_CAIRO_MAX ? MTK_CAIRO_MAX : win->h * s;
	if (bh > INT32_MAX / 4 / bw)
		bh = INT32_MAX / 4 / bw;
	struct shmbuf *b = get_buffer(win, bw, bh);
	if (!b)
		return 0;
	cairo_t *cr = b->cr;
	cairo_save(cr);
	cairo_identity_matrix(cr);
	cairo_scale(cr, s, s);
	win->l->paint(win, cr, win->w, win->h, win->data);
	cairo_restore(cr);
	cairo_surface_flush(b->cs);
	win->paints++;
	if (win->frame)
		wl_callback_destroy(win->frame);
	win->frame = wl_surface_frame(win->surface);
	wl_callback_add_listener(win->frame, &frame_listener, win);
	wl_surface_set_buffer_scale(win->surface, s);
	wl_surface_attach(win->surface, b->wl, 0, 0);
	damage(win, b, s);
	wl_surface_commit(win->surface);
	b->busy = 1;
	win->shown = b;
	win->shown_scale = s;
	return 1;
}

/* ---- the cursor ----------------------------------------------------------- */

/* The CSS names first, then the X ones older themes have. */
static struct wl_cursor *find_cursor(struct wl_cursor_theme *t, const char *name)
{
	static const char *const alias[][2] = {
		{ "default", "left_ptr" }, { "text", "xterm" }, { "pointer", "hand2" },
		{ "wait", "watch" }, { "move", "fleur" }, { "not-allowed", "crossed_circle" },
		{ "col-resize", "sb_h_double_arrow" }, { "row-resize", "sb_v_double_arrow" },
	};
	struct wl_cursor *c = wl_cursor_theme_get_cursor(t, name);
	for (size_t i = 0; !c && i < sizeof alias / sizeof alias[0]; i++)
		if (!strcmp(name, alias[i][0]))
			c = wl_cursor_theme_get_cursor(t, alias[i][1]);
	return c;
}

/* The cursor on the window, if the pointer is there; 0 if not shown. */
static int cursor_show(struct mtk *m)
{
	struct wl_cursor_image *img = m->cursor_img;
	if (!m->pointer || !m->ptr_win || !img)
		return 0;
	int d = m->cursor_buf_scale;
	int hx = img->hotspot_x / d, hy = img->hotspot_y / d;
	if (m->cursor_vw > 0) {
		/* Rounded as the viewport's size is. */
		hx = (img->hotspot_x * m->cursor_vw + img->width / 2) / img->width;
		hy = (img->hotspot_y * m->cursor_vh + img->height / 2) / img->height;
	}
	wl_pointer_set_cursor(m->pointer, m->ptr_serial, m->cursor_surf, hx, hy);
	if (m->cursor_vp)
		wp_viewport_set_destination(m->cursor_vp, m->cursor_vw, m->cursor_vh);
	wl_surface_set_buffer_scale(m->cursor_surf, d);
	wl_surface_attach(m->cursor_surf, wl_cursor_image_get_buffer(img), 0, 0);
	wl_surface_damage_buffer(m->cursor_surf, 0, 0, img->width, img->height);
	wl_surface_commit(m->cursor_surf);
	return 1;
}

/* The window's cursor at its scale, loaded again when either changes, and shown
 * if the pointer is on it. A failed load keeps the old cursor and is tried again
 * on the next enter. The old theme's buffers go with it, so the surface shows
 * the new cursor or none before it goes. */
static void cursor_update(struct mtk *m)
{
	struct mtk_window *win = m->ptr_win ? m->ptr_win : m->windows;
	int scale = win ? win->scale : 1;
	const char *name = win && win->cursor ? win->cursor : "default";
	if (m->cursor_scale != scale || strcmp(m->cursor_name, name)) {
		struct wl_cursor_theme *t = m->cursor_theme;
		if (m->cursor_scale != scale) {
			t = wl_cursor_theme_load(getenv("XCURSOR_THEME"),
				mtk_cursor_size(getenv("XCURSOR_SIZE")) * scale, m->shm);
			m->cursor_loads++;
		}
		struct wl_cursor *c = t ? find_cursor(t, name) : NULL;
		if (!c && t && strcmp(name, "default"))
			c = find_cursor(t, "default");
		if (c) {
			struct wl_cursor_theme *old = t != m->cursor_theme ?
				m->cursor_theme : NULL;
			m->cursor_theme = t;
			m->cursor_img = c->images[0];
			m->cursor_scale = scale;
			snprintf(m->cursor_name, sizeof m->cursor_name, "%s", name);
			if (!m->cursor_surf)
				m->cursor_surf = wl_compositor_create_surface(m->compositor);
			if (m->viewporter && !m->cursor_vp)
				m->cursor_vp = wp_viewporter_get_viewport(m->viewporter,
					m->cursor_surf);
			mtk_cursor_fit(m->cursor_img->width, m->cursor_img->height, scale,
				!!m->cursor_vp, &m->cursor_buf_scale, &m->cursor_vw,
				&m->cursor_vh);
			if (!cursor_show(m) && old) {
				wl_surface_attach(m->cursor_surf, NULL, 0, 0);
				wl_surface_commit(m->cursor_surf);
			}
			if (old)
				wl_cursor_theme_destroy(old);
			return;
		}
		if (t && t != m->cursor_theme)
			wl_cursor_theme_destroy(t);
	}
	cursor_show(m);
}

/* ---- the keyboard --------------------------------------------------------- */

/* The keys go to the newest popup with the grab over the window that has the
 * keyboard: a compositor may leave the keyboard with that window, as mwc does. */
static struct mtk_window *key_target(struct mtk *m)
{
	for (struct mtk_window *w = m->windows; w && m->kb_win; w = w->next) {
		if (!w->grab)
			continue;
		for (struct mtk_window *a = w; a; a = a->parent)
			if (a == m->kb_win)
				return w;
	}
	return m->kb_win;
}

static int key_make(struct mtk *m, uint32_t kc, int repeat, struct mtk_key *k,
	char utf8[64])
{
	if (!mtk_key_translate(m->kb_state, m->compose, kc, &k->sym, utf8))
		return 0;
	k->kc = kc;
	k->latin = mtk_latin_sym(m->kb_map, kc, k->sym);
	k->utf8 = utf8;
	k->mods = mtk_mods(m->kb_state);
	k->repeat = repeat;
	return 1;
}

static int key_may_repeat(struct mtk_window *win, const struct mtk_key *k)
{
	return !win->l->key_repeats || win->l->key_repeats(win, k, win->data);
}

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
	int32_t fd, uint32_t size)
{
	struct mtk *m = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *s = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (s == MAP_FAILED)
		return;
	/* Only up to size: the NUL at the end is a custom, not a rule. */
	struct xkb_keymap *map = xkb_keymap_new_from_buffer(m->xkb, s, strnlen(s, size),
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(s, size);
	struct xkb_state *state = map ? xkb_state_new(map) : NULL;
	if (!state) {
		xkb_keymap_unref(map);
		return;
	}
	xkb_state_unref(m->kb_state);
	xkb_keymap_unref(m->kb_map);
	m->kb_state = state;
	m->kb_map = map;
	mtk_repeat_stop(&m->repeat);
}

static struct mtk_window *window_of(struct mtk *m, struct wl_surface *s)
{
	for (struct mtk_window *w = m->windows; w; w = w->next)
		if (s && w->surface == s)
			return w;
	return NULL;
}

static void set_focus(struct mtk_window *win, int in)
{
	if (!win || win->focused == in)
		return;
	win->focused = in;
	if (win->l->focus)
		win->l->focus(win, in, win->data);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
	struct wl_surface *s, struct wl_array *keys)
{
	struct mtk *m = data;
	m->serial = serial;
	m->kb_win = window_of(m, s);
	set_focus(m->kb_win, 1);
}

static void kb_lost(struct mtk *m)
{
	set_focus(m->kb_win, 0);
	m->kb_win = NULL;
	mtk_repeat_stop(&m->repeat);
	if (m->compose)
		xkb_compose_state_reset(m->compose);
	/* What is held now is unknown, and a click can come before the next enter,
	 * which brings the real state. */
	if (m->kb_state)
		xkb_state_update_mask(m->kb_state, 0, 0, 0, 0, 0,
			xkb_state_serialize_layout(m->kb_state, XKB_STATE_LAYOUT_EFFECTIVE));
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
	struct wl_surface *s)
{
	kb_lost(data);
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	struct mtk *m = data;
	uint32_t kc = code + 8;
	struct mtk_window *win = key_target(m);
	m->serial = serial;
	if (!m->kb_state)
		return;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		mtk_repeat_release(&m->repeat, kc);
		return;
	}
	struct mtk_key k;
	char utf8[64];
	int repeats = xkb_keymap_key_repeats(m->kb_map, kc);
	int made = key_make(m, kc, 0, &k, utf8);
	/* A modifier does not stop a key that is repeating; a key in a compose
	 * sequence does. */
	mtk_repeat_press(&m->repeat, kc, repeats, made && win && key_may_repeat(win, &k),
		mtk_now());
	if (made && win && win->l->key)
		win->l->key(win, &k, win->data);
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	struct mtk *m = data;
	if (m->kb_state)
		xkb_state_update_mask(m->kb_state, depressed, latched, locked, 0, 0,
			group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
	int32_t delay)
{
	struct mtk *m = data;
	mtk_repeat_info(&m->repeat, rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

/* The key held, again; the key may mean something else by now. */
static void key_repeat(struct mtk *m)
{
	uint32_t kc = mtk_repeat_due(&m->repeat, mtk_now());
	struct mtk_window *win = key_target(m);
	if (!kc || !win || !m->kb_state)
		return;
	struct mtk_key k;
	char utf8[64];
	if (!key_make(m, kc, 1, &k, utf8) || !key_may_repeat(win, &k)) {
		mtk_repeat_stop(&m->repeat);
		return;
	}
	if (win->l->key)
		win->l->key(win, &k, win->data);
}

/* Off the window, or the pointer gone: no scrolling carries over. */
static void scroll_reset(struct mtk *m)
{
	m->axis = 0;
	m->scroll_x = m->scroll_y = (struct mtk_scroll){ 0 };
	m->dx = m->dy = 0;
}

/* ---- the pointer ---------------------------------------------------------- */

static void ptr_send(struct mtk *m, enum mtk_pointer_kind kind, uint32_t button,
	uint32_t serial, uint32_t time)
{
	struct mtk_window *win = m->ptr_win;
	if (!win || !win->l->pointer)
		return;
	struct mtk_pointer p = {
		.kind = kind, .x = m->ptr_x, .y = m->ptr_y, .button = button,
		.serial = serial, .time = time, .mods = mtk_mods(m->kb_state),
		.buttons = m->buttons,
	};
	if (kind == MTK_SCROLL) {
		p.steps_x = mtk_scroll_frame(&m->scroll_x);
		p.steps_y = mtk_scroll_frame(&m->scroll_y);
		p.dx = m->dx;
		p.dy = m->dy;
	}
	win->l->pointer(win, &p, win->data);
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	struct mtk *m = data;
	m->ptr_serial = serial;
	m->ptr_win = window_of(m, s);
	m->ptr_x = wl_fixed_to_double(x);
	m->ptr_y = wl_fixed_to_double(y);
	cursor_update(m);
	ptr_send(m, MTK_ENTER, 0, serial, 0);
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s)
{
	struct mtk *m = data;
	ptr_send(m, MTK_LEAVE, 0, serial, 0);
	m->ptr_win = NULL;
	m->buttons = 0;
	scroll_reset(m);
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
	wl_fixed_t x, wl_fixed_t y)
{
	struct mtk *m = data;
	m->ptr_x = wl_fixed_to_double(x);
	m->ptr_y = wl_fixed_to_double(y);
	ptr_send(m, MTK_MOTION, 0, 0, time);
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state)
{
	struct mtk *m = data;
	int pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	unsigned bit = button == BTN_LEFT ? MTK_BUTTON_LEFT : button == BTN_MIDDLE ?
		MTK_BUTTON_MIDDLE : button == BTN_RIGHT ? MTK_BUTTON_RIGHT : 0;
	m->serial = serial;
	ptr_send(m, pressed ? MTK_PRESS : MTK_RELEASE, button, serial, time);
	/* A press sees the buttons held before it, as with any toolkit. */
	m->buttons = pressed ? m->buttons | bit : m->buttons & ~bit;
}

static void ptr_frame(void *data, struct wl_pointer *p)
{
	struct mtk *m = data;
	if (m->axis)
		ptr_send(m, MTK_SCROLL, 0, 0, m->axis_time);
	m->axis = 0;
	m->dx = m->dy = 0;
}

static struct mtk_scroll *scroll_of(struct mtk *m, uint32_t axis)
{
	return axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? &m->scroll_y : &m->scroll_x;
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	struct mtk *m = data;
	double v = wl_fixed_to_double(value);
	mtk_scroll_value(scroll_of(m, axis), v);
	*(axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? &m->dy : &m->dx) += v;
	m->axis = 1;
	m->axis_time = time;
	/* Before wl_seat 5 no frame ends the event: each one counts by itself. */
	if (wl_pointer_get_version(p) < WL_POINTER_FRAME_SINCE_VERSION)
		ptr_frame(data, p);
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
	int32_t steps)
{
	struct mtk *m = data;
	mtk_scroll_discrete(scroll_of(m, axis), steps);
	m->axis = 1;
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis)
{
	mtk_scroll_stop(scroll_of(data, axis));
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

/* ---- the seat ------------------------------------------------------------- */

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	struct mtk *m = data;
	int want_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
	int want_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (want_ptr && !m->pointer) {
		m->pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(m->pointer, &pointer_listener, m);
	} else if (!want_ptr && m->pointer) {
		/* Gone without a leave, perhaps: the window hears of it all the same. */
		ptr_send(m, MTK_LEAVE, 0, 0, 0);
		m->buttons = 0;
		if (wl_pointer_get_version(m->pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
			wl_pointer_release(m->pointer);
		else
			wl_pointer_destroy(m->pointer);
		m->pointer = NULL;
		m->ptr_win = NULL;
		scroll_reset(m);
	}
	if (want_kb && !m->keyboard) {
		m->keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(m->keyboard, &keyboard_listener, m);
	} else if (!want_kb && m->keyboard) {
		if (wl_keyboard_get_version(m->keyboard) >=
		    WL_KEYBOARD_RELEASE_SINCE_VERSION)
			wl_keyboard_release(m->keyboard);
		else
			wl_keyboard_destroy(m->keyboard);
		m->keyboard = NULL;
		kb_lost(m);     /* in case no leave came first */
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void seat_bind(struct mtk *m, uint32_t name, uint32_t ver)
{
	m->seat = wl_registry_bind(m->registry, name, &wl_seat_interface, MIN(ver, 7));
	m->seat_global = name;
	wl_seat_add_listener(m->seat, &seat_listener, m);
}

static void seat_drop(struct mtk *m)
{
	seat_capabilities(m, m->seat, 0);
	if (wl_seat_get_version(m->seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
		wl_seat_release(m->seat);
	else
		wl_seat_destroy(m->seat);
	m->seat = NULL;
}

/* ---- the registry --------------------------------------------------------- */

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_ping };

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	struct mtk *m = data;
	/* damage_buffer needs wl_compositor 4, preferred_buffer_scale 6. */
	if (!strcmp(iface, wl_compositor_interface.name) && ver >= 4 && !m->compositor) {
		m->compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			MIN(ver, 6));
	} else if (!strcmp(iface, wl_shm_interface.name) && !m->shm) {
		m->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		if (mtk_seats_add(&m->seats, name, ver) && !m->seat)
			seat_bind(m, name, ver);
	} else if (!strcmp(iface, wp_viewporter_interface.name) && !m->viewporter) {
		m->viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name) && !m->wm_base) {
		m->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, MIN(ver, 2));
		xdg_wm_base_add_listener(m->wm_base, &wm_base_listener, m);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name) &&
		   !m->deco_mgr) {
		m->deco_mgr = wl_registry_bind(r, name,
			&zxdg_decoration_manager_v1_interface, 1);
	} else if (m->cfg.global) {
		m->cfg.global(m, r, name, iface, ver, m->cfg.data);
	}
}

/* A seat that goes is let go of, and the oldest one left taken; with none, the
 * next one announced. */
static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
	struct mtk *m = data;
	mtk_seats_remove(&m->seats, name);
	if (m->seat && name == m->seat_global) {
		seat_drop(m);
		unsigned next, ver;
		if (mtk_seats_next(&m->seats, &next, &ver))
			seat_bind(m, next, ver);
		return;
	}
	if (m->cfg.global_remove)
		m->cfg.global_remove(m, name, m->cfg.data);
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

struct mtk *mtk_open(const struct mtk_config *cfg)
{
	struct mtk *m = calloc(1, sizeof *m);
	if (!m) {
		fprintf(stderr, "%s: out of memory\n", cfg->name);
		return NULL;
	}
	m->cfg = *cfg;
	mtk_repeat_init(&m->repeat);
	if (!(m->dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "%s: cannot connect to a Wayland compositor\n", cfg->name);
		free(m);
		return NULL;
	}
	if (!(m->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		fprintf(stderr, "%s: cannot set up xkbcommon\n", cfg->name);
		mtk_close(m);
		return NULL;
	}
	m->compose = mtk_compose_new(m->xkb);
	m->registry = wl_display_get_registry(m->dpy);
	wl_registry_add_listener(m->registry, &registry_listener, m);
	if (wl_display_roundtrip(m->dpy) < 0)
		lost_connection(m);
	if (!m->compositor || !m->shm || !m->wm_base) {
		fprintf(stderr, "%s: the compositor lacks wl_compositor 4, wl_shm or "
			"xdg_wm_base\n", cfg->name);
		mtk_close(m);
		return NULL;
	}
	return m;
}

void mtk_close(struct mtk *m)
{
	if (!m)
		return;
	while (m->windows)
		mtk_window_destroy(m->windows);
	while (m->stale)
		buf_free(m->stale);
	if (m->seat)
		seat_drop(m);
	if (m->cursor_vp)
		wp_viewport_destroy(m->cursor_vp);
	if (m->cursor_surf)
		wl_surface_destroy(m->cursor_surf);
	if (m->cursor_theme)
		wl_cursor_theme_destroy(m->cursor_theme);
	if (m->deco_mgr)
		zxdg_decoration_manager_v1_destroy(m->deco_mgr);
	if (m->viewporter)
		wp_viewporter_destroy(m->viewporter);
	if (m->wm_base)
		xdg_wm_base_destroy(m->wm_base);
	if (m->shm)
		wl_shm_destroy(m->shm);
	if (m->compositor)
		wl_compositor_destroy(m->compositor);
	if (m->registry)
		wl_registry_destroy(m->registry);
	xkb_compose_state_unref(m->compose);
	xkb_state_unref(m->kb_state);
	xkb_keymap_unref(m->kb_map);
	xkb_context_unref(m->xkb);
	wl_display_disconnect(m->dpy);
	free(m);
}

struct wl_display *mtk_display(struct mtk *m)
{
	return m->dpy;
}

struct wl_seat *mtk_seat(struct mtk *m)
{
	return m->seat;
}

uint32_t mtk_serial(struct mtk *m)
{
	return m->serial;
}

/* ---- windows -------------------------------------------------------------- */

static void xdg_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	struct mtk_window *win = data;
	xdg_surface_ack_configure(s, serial);
	win->states = win->cfg_states;
	if (!win->fixed && win->toplevel)
		mtk_configure_size(win->cfg_w, win->cfg_h, win->states, win->min_w,
			win->min_h, &win->free_w, &win->free_h, &win->w, &win->h);
	if (win->l->configure)
		win->l->configure(win, win->cfg_w, win->cfg_h, win->cfg_states, win->data);
	win->configured = 1;
	win->need_commit = 1;
	win->dirty = 1;
}

static const struct xdg_surface_listener xdg_listener = { xdg_configure };

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
	struct wl_array *states)
{
	struct mtk_window *win = data;
	unsigned st = 0;
	uint32_t *s;
	wl_array_for_each(s, states) {
		switch (*s) {
		case XDG_TOPLEVEL_STATE_ACTIVATED: st |= MTK_STATE_ACTIVATED; break;
		case XDG_TOPLEVEL_STATE_MAXIMIZED: st |= MTK_STATE_MAXIMIZED; break;
		case XDG_TOPLEVEL_STATE_FULLSCREEN: st |= MTK_STATE_FULLSCREEN; break;
		case XDG_TOPLEVEL_STATE_TILED_LEFT: case XDG_TOPLEVEL_STATE_TILED_RIGHT:
		case XDG_TOPLEVEL_STATE_TILED_TOP: case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
			st |= MTK_STATE_TILED;
			break;
		}
	}
	win->cfg_w = w;
	win->cfg_h = h;
	win->cfg_states = st;
}

static void top_close(void *data, struct xdg_toplevel *t)
{
	struct mtk_window *win = data;
	if (win->l->close)
		win->l->close(win, win->data);
	else
		mtk_quit(win->m, 0);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = top_configure,
	.close = top_close,
};

/* Sharp on a scaled output: wl_compositor 6 says which scale the output it is on
 * has. An older compositor never does, and it stays at 1. */
static void surf_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
}

static void surf_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
}

static void surf_preferred_scale(void *data, struct wl_surface *s, int32_t factor)
{
	struct mtk_window *win = data;
	factor = mtk_buffer_scale(factor);
	if (!factor || factor == win->scale)
		return;
	win->scale = factor;
	win->dirty = 1;
	if (win->l->scale)
		win->l->scale(win, factor, win->data);
	/* Now, not at the next enter: the pointer may be resting on the window. */
	if (win->m->ptr_win == win || !win->m->ptr_win)
		cursor_update(win->m);
}

static void surf_preferred_transform(void *data, struct wl_surface *s,
	uint32_t transform)
{
}

static const struct wl_surface_listener surf_listener = {
	.enter = surf_enter,
	.leave = surf_leave,
	.preferred_buffer_scale = surf_preferred_scale,
	.preferred_buffer_transform = surf_preferred_transform,
};

/* What the compositor is told of the size, only when it changes. */
static void tell_size(struct mtk_window *win)
{
	int min_w = win->fixed ? win->w : win->min_w;
	int min_h = win->fixed ? win->h : win->min_h;
	if (!win->toplevel || (min_w == win->sized_w && min_h == win->sized_h))
		return;
	win->sized_w = min_w;
	win->sized_h = min_h;
	xdg_toplevel_set_min_size(win->toplevel, min_w, min_h);
	if (win->fixed)
		xdg_toplevel_set_max_size(win->toplevel, min_w, min_h);
}

struct mtk_window *mtk_window_new(struct mtk *m, const struct mtk_window_config *c,
	const struct mtk_window_listener *l, void *data)
{
	struct mtk_window *win = calloc(1, sizeof *win);
	if (!win) {
		fprintf(stderr, "%s: out of memory\n", m->cfg.name);
		exit(1);
	}
	const char *app_id = c->app_id ? c->app_id : m->cfg.name;
	*win = (struct mtk_window){
		.m = m, .l = l, .data = data, .scale = 1, .fixed = c->fixed,
		.w = c->width > 0 ? c->width : 100, .h = c->height > 0 ? c->height : 100,
		.min_w = c->min_width, .min_h = c->min_height, .sized_w = -1,
		.sized_h = -1,
	};
	win->free_w = win->w;
	win->free_h = win->h;
	win->next = m->windows;
	m->windows = win;
	win->surface = wl_compositor_create_surface(m->compositor);
	wl_surface_add_listener(win->surface, &surf_listener, win);
	win->xdg = xdg_wm_base_get_xdg_surface(m->wm_base, win->surface);
	xdg_surface_add_listener(win->xdg, &xdg_listener, win);
	win->toplevel = xdg_surface_get_toplevel(win->xdg);
	xdg_toplevel_add_listener(win->toplevel, &toplevel_listener, win);
	xdg_toplevel_set_app_id(win->toplevel, app_id);
	xdg_toplevel_set_title(win->toplevel, c->title ? c->title : app_id);
	tell_size(win);
	/* Without it a compositor that offers xdg-decoration may add a title bar
	 * or not, as it likes; mwc adds none. */
	if (c->server_decoration && m->deco_mgr) {
		win->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(
			m->deco_mgr, win->toplevel);
		zxdg_toplevel_decoration_v1_set_mode(win->deco,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	if (c->maximized)
		xdg_toplevel_set_maximized(win->toplevel);
	wl_surface_commit(win->surface);
	return win;
}

static struct mtk_window *popup_of(struct mtk *m, struct mtk_window *win)
{
	for (struct mtk_window *c = m->windows; c; c = c->next)
		if (c->parent == win)
			return c;
	return NULL;
}

void mtk_window_destroy(struct mtk_window *win)
{
	struct mtk *m = win->m;
	struct mtk_window *c;
	/* Its popups first: a parent gone before them is a protocol error. */
	while ((c = popup_of(m, win)))
		mtk_window_destroy(c);
	for (struct mtk_window **p = &m->windows; *p; p = &(*p)->next)
		if (*p == win) {
			*p = win->next;
			break;
		}
	if (m->kb_win == win)
		m->kb_win = NULL;
	if (m->ptr_win == win)
		m->ptr_win = NULL;
	if (win->frame)
		wl_callback_destroy(win->frame);
	for (int i = 0; i < 2; i++)
		if (win->bufs[i])
			buf_drop(win->bufs[i]);
	if (win->deco)
		zxdg_toplevel_decoration_v1_destroy(win->deco);
	if (win->toplevel)
		xdg_toplevel_destroy(win->toplevel);
	if (win->popup)
		xdg_popup_destroy(win->popup);
	xdg_surface_destroy(win->xdg);
	wl_surface_destroy(win->surface);
	free(win->cursor);
	free(win);
}

void mtk_window_redraw(struct mtk_window *win)
{
	win->dirty = 1;
}

void mtk_window_set_size(struct mtk_window *win, int w, int h)
{
	if (w < 1 || h < 1 || (w == win->w && h == win->h && win->sized_w >= 0))
		return;
	win->w = w;
	win->h = h;
	if (!(win->states & (MTK_STATE_MAXIMIZED | MTK_STATE_FULLSCREEN | MTK_STATE_TILED))) {
		win->free_w = w;
		win->free_h = h;
	}
	tell_size(win);
	win->dirty = 1;
}

void mtk_window_set_min_size(struct mtk_window *win, int w, int h)
{
	win->min_w = w;
	win->min_h = h;
	tell_size(win);
}

void mtk_window_set_title(struct mtk_window *win, const char *title)
{
	if (win->toplevel)
		xdg_toplevel_set_title(win->toplevel, title);
}

static void popup_configure(void *data, struct xdg_popup *p, int32_t x, int32_t y,
	int32_t w, int32_t h)
{
	struct mtk_window *win = data;
	win->pop_x = x;
	win->pop_y = y;
}

/* A click on another client, or the compositor refused the grab. */
static void popup_done(void *data, struct xdg_popup *p)
{
	struct mtk_window *win = data;
	if (win->l->close)
		win->l->close(win, win->data);
	else
		mtk_window_destroy(win);
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
};

struct mtk_window *mtk_popup_new(struct mtk_window *parent,
	const struct mtk_popup_config *c, const struct mtk_window_listener *l,
	void *data)
{
	struct mtk *m = parent->m;
	struct mtk_window *win = calloc(1, sizeof *win);
	if (!win) {
		fprintf(stderr, "%s: out of memory\n", m->cfg.name);
		exit(1);
	}
	/* At the parent's scale until the compositor says which. */
	*win = (struct mtk_window){
		.m = m, .l = l, .data = data, .parent = parent, .fixed = 1,
		.scale = parent->scale, .w = c->width > 0 ? c->width : 1,
		.h = c->height > 0 ? c->height : 1, .pop_x = c->x, .pop_y = c->y,
	};
	win->next = m->windows;
	m->windows = win;
	win->surface = wl_compositor_create_surface(m->compositor);
	wl_surface_add_listener(win->surface, &surf_listener, win);
	win->xdg = xdg_wm_base_get_xdg_surface(m->wm_base, win->surface);
	xdg_surface_add_listener(win->xdg, &xdg_listener, win);
	/* Below and right of the point, else above or left of it with the point still
	 * on the corner, and always on the screen. */
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(m->wm_base);
	xdg_positioner_set_size(pos, win->w, win->h);
	int ax = c->x < 0 ? 0 : c->x >= parent->w ? parent->w - 1 : c->x;
	int ay = c->y < 0 ? 0 : c->y >= parent->h ? parent->h - 1 : c->y;
	xdg_positioner_set_anchor_rect(pos, ax, ay, 1, 1);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(pos,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
	win->popup = xdg_surface_get_popup(win->xdg, parent->xdg, pos);
	xdg_positioner_destroy(pos);
	xdg_popup_add_listener(win->popup, &popup_listener, win);
	if (c->grab && m->seat) {
		xdg_popup_grab(win->popup, m->seat, c->serial);
		win->grab = 1;
	}
	wl_surface_commit(win->surface);
	return win;
}

void mtk_popup_position(const struct mtk_window *win, int *x, int *y)
{
	*x = win->pop_x;
	*y = win->pop_y;
}

void mtk_window_set_cursor(struct mtk_window *win, const char *name)
{
	const char *now = win->cursor ? win->cursor : "default";
	if (!name)
		name = "default";
	if (!strcmp(now, name))
		return;
	free(win->cursor);
	win->cursor = strcmp(name, "default") ? strdup(name) : NULL;
	if (win->m->ptr_win == win)
		cursor_update(win->m);
}

void mtk_window_size(const struct mtk_window *win, int *w, int *h)
{
	*w = win->w;
	*h = win->h;
}

int mtk_window_scale(const struct mtk_window *win)
{
	return win->scale;
}

int mtk_window_focused(const struct mtk_window *win)
{
	return win->focused;
}

struct mtk *mtk_window_mtk(const struct mtk_window *win)
{
	return win->m;
}

struct wl_surface *mtk_window_surface(const struct mtk_window *win)
{
	return win->surface;
}

struct xdg_toplevel *mtk_window_toplevel(const struct mtk_window *win)
{
	return win->toplevel;
}

unsigned mtk_window_paints(const struct mtk_window *win)
{
	return win->paints;
}

int mtk_window_dirty(const struct mtk_window *win)
{
	return win->dirty;
}

unsigned mtk_cursor_loads(struct mtk *m)
{
	return m->cursor_loads;
}

/* ---- the loop ------------------------------------------------------------- */

int mtk_watch_fd(struct mtk *m, int fd, short events, mtk_fd_fn fn, void *data)
{
	for (int i = 0; i < m->nwatch; i++)
		if (m->watch[i].fd == fd) {
			m->watch[i] = (struct watch){ fd, events, fn, data };
			return 0;
		}
	if (m->nwatch == MAX_WATCH)
		return -1;
	m->watch[m->nwatch++] = (struct watch){ fd, events, fn, data };
	return 0;
}

void mtk_unwatch_fd(struct mtk *m, int fd)
{
	for (int i = 0; i < m->nwatch; i++)
		if (m->watch[i].fd == fd) {
			m->watch[i] = m->watch[--m->nwatch];
			return;
		}
}

unsigned mtk_timer(struct mtk *m, int ms, mtk_timer_fn fn, void *data)
{
	for (int i = 0; i < MAX_TIMERS; i++)
		if (!m->timers[i].id) {
			if (!++m->timer_id)
				m->timer_id = 1;
			m->timers[i] = (struct timer){ m->timer_id,
				mtk_now() + (uint64_t)(ms > 0 ? ms : 0), fn, data };
			return m->timer_id;
		}
	return 0;
}

void mtk_timer_cancel(struct mtk *m, unsigned id)
{
	for (int i = 0; id && i < MAX_TIMERS; i++)
		if (m->timers[i].id == id)
			m->timers[i].id = 0;
}

/* Each timer that is due, once; one it sets anew waits for the next round. */
static void run_timers(struct mtk *m)
{
	uint64_t now = mtk_now();
	struct timer due[MAX_TIMERS];
	int n = 0;
	for (int i = 0; i < MAX_TIMERS; i++)
		if (m->timers[i].id && m->timers[i].at <= now) {
			due[n++] = m->timers[i];
			m->timers[i].id = 0;
		}
	for (int i = 0; i < n; i++)
		due[i].fn(m, due[i].data);
}

static int next_timeout(struct mtk *m)
{
	uint64_t now = mtk_now();
	int t = mtk_repeat_timeout(&m->repeat, now);
	for (int i = 0; i < MAX_TIMERS; i++) {
		if (!m->timers[i].id)
			continue;
		uint64_t at = m->timers[i].at;
		int d = at <= now ? 0 : at - now > INT32_MAX ? INT32_MAX : (int)(at - now);
		if (t < 0 || d < t)
			t = d;
	}
	return t;
}

/* A frame at a time, except that a configure wants its commit even if the window
 * is hidden and gets no frames. */
static void draw_windows(struct mtk *m)
{
	for (struct mtk_window *w = m->windows; w; w = w->next)
		if (w->dirty && w->configured && (!w->frame || w->need_commit) &&
		    draw(w)) {
			w->dirty = 0;
			w->need_commit = 0;
		}
}

void mtk_before_poll(struct mtk *m, struct pollfd *pfd, int *timeout)
{
	for (;;) {
		if (wl_display_dispatch_pending(m->dpy) < 0)
			lost_connection(m);
		key_repeat(m);
		run_timers(m);
		draw_windows(m);
		if (m->cfg.idle)
			m->cfg.idle(m, m->cfg.data);
		if (wl_display_prepare_read(m->dpy) == 0)
			break;
		/* events arrived in the meantime */
	}
	m->reading = 1;
	*pfd = (struct pollfd){ .fd = wl_display_get_fd(m->dpy), .events = POLLIN };
	/* EPIPE: the compositor has closed, but may have said why first. Reading
	 * that error makes libwayland report it. */
	if (wl_display_flush(m->dpy) < 0) {
		if (errno == EAGAIN) {
			pfd->events |= POLLOUT;
		} else if (errno != EPIPE) {
			wl_display_cancel_read(m->dpy);
			m->reading = 0;
			lost_connection(m);
		}
	}
	int t = next_timeout(m);
	if (t >= 0 && (*timeout < 0 || t < *timeout))
		*timeout = t;
}

void mtk_after_poll(struct mtk *m, short revents)
{
	if (!m->reading)
		return;
	m->reading = 0;
	if (revents & (POLLIN | POLLERR | POLLHUP)) {
		if (wl_display_read_events(m->dpy) < 0)
			lost_connection(m);
	} else {
		wl_display_cancel_read(m->dpy);
	}
	if (wl_display_dispatch_pending(m->dpy) < 0)
		lost_connection(m);
	key_repeat(m);
	run_timers(m);
}

int mtk_run(struct mtk *m)
{
	m->quit = 0;
	while (!m->quit) {
		struct pollfd pfd[1 + MAX_WATCH];
		struct watch w[MAX_WATCH];
		int timeout = -1, n = m->nwatch;
		mtk_before_poll(m, &pfd[0], &timeout);
		if (m->quit) {
			mtk_after_poll(m, 0);
			break;
		}
		memcpy(w, m->watch, n * sizeof *w);
		for (int i = 0; i < n; i++)
			pfd[1 + i] = (struct pollfd){ .fd = w[i].fd, .events = w[i].events };
		if (poll(pfd, 1 + n, timeout) < 0) {
			if (errno != EINTR) {
				fprintf(stderr, "%s: poll: %s\n", m->cfg.name, strerror(errno));
				exit(1);
			}
			for (int i = 0; i <= n; i++)
				pfd[i].revents = 0;
		}
		mtk_after_poll(m, pfd[0].revents);
		/* A callback may unwatch another fd: only those still watched. */
		for (int i = 0; i < n && !m->quit; i++) {
			if (!pfd[1 + i].revents)
				continue;
			for (int j = 0; j < m->nwatch; j++)
				if (m->watch[j].fd == w[i].fd) {
					struct watch c = m->watch[j];
					c.fn(m, c.fd, pfd[1 + i].revents, c.data);
					break;
				}
		}
	}
	return m->status;
}

void mtk_quit(struct mtk *m, int status)
{
	m->quit = 1;
	m->status = status;
}
