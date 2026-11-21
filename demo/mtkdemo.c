/*
 * mtkdemo - a window with what mtk does: mwm's thin border in the colour of the
 * focus, a button, a field to type into, scrolling, a timer and an fd. tests/ui.sh
 * drives it in a headless mwc and reads what it did from MTKDEMO_STATE.
 *
 *   MTKDEMO_STATE=FILE   "key value" lines, rewritten whole on each change
 *   MTKDEMO_FIXED=1      always 300x120, as a dock tool
 *   MTKDEMO_WIDTH=N      N wide instead of 300
 *   MTKDEMO_DECO=1       ask for mwc's frame
 *   MTKDEMO_MAXIMIZED=1  maximized from the start
 *   MTKDEMO_OWN_LOOP=1   its own poll around mtk_before_poll() and mtk_after_poll(),
 *                        as a client with another library's loop has it
 *   MTKDEMO_DROP_MENU=1  the menu closes itself as soon as it is configured
 *
 * Keys: text into the field, BackSpace, Esc or Ctrl+Q quits, F2 makes it 360x140
 * with a minimum of 250x110 and the title "mtkdemo 360". SIGUSR1 counts through a
 * pipe the loop watches. SIGUSR2 starts two timers due at once that cancel each
 * other, and stops watching a second pipe from the idle hook. The right button
 * opens a menu of two items: click one, or Up / Down and Return; Esc or a click
 * outside closes it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "mtk.h"

#define PAD 6
#define BTN_W 80

static struct mtk *m;
static struct mtk_window *win;
static struct mtk_theme theme;
static PangoLayout *lo;
static int barh;
static char text[256];
static unsigned keys, repeats, clicks, signals, ticks, scrolls, sigcalls, races;
static int steps, pressed, press_x, press_y, focused, scale = 1;
static const char *state_path;
static int sig_pipe[2] = { -1, -1 };
static int extra_pipe[2] = { -1, -1 };  /* watched until SIGUSR2 */
static int unwatch_extra, drop_menu;
static unsigned race_a, race_b, menu_configs;
static uint64_t started;         /* mtk_now() at the start */
static int done;                /* asked to quit, for the own loop */
static struct mtk_window *menu; /* the popup, while it is up */
static int menu_hot = -1, choice, press_buttons, press_mods;
#define MENU_W 120
#define ITEM_H 24

static void state_write(void)
{
	static char last[1024];
	char now[sizeof last], tmp[4096];
	int w, h;
	if (!state_path)
		return;
	mtk_window_size(win, &w, &h);
	int px = -1, py = -1;
	if (menu)
		mtk_popup_position(menu, &px, &py);
	/* What mtk hands out of a window: its mtk, surface and toplevel, none for a
	 * popup; and a clock that doesn't go back. */
	int handles = mtk_window_mtk(win) == m && mtk_window_surface(win) &&
		mtk_window_toplevel(win) && mtk_now() >= started &&
		(!menu || (mtk_window_surface(menu) && !mtk_window_toplevel(menu)));
	snprintf(now, sizeof now, "focus %d\nscale %d\nsize %dx%d\npaints %u\nkeys %u\n"
		"repeats %u\nclicks %u\npress %d %d\nscrolls %u\nsteps %d\nsignals %u\n"
		"ticks %u\ndirty %d\ncursors %u\npopup %d\nchoice %d\nbuttons %d\nmods %d\n"
		"popxy %d %d\nsigcalls %u\nraces %u\nwscale %d\nwfocus %d\nseat %d\n"
		"display %d\nserial %d\nmenucfg %u\nhandles %d\ntext %s\n", focused, scale, w, h,
		mtk_window_paints(win), keys, repeats, clicks, press_x, press_y, scrolls, steps,
		signals, ticks, mtk_window_dirty(win), mtk_cursor_loads(m), menu != NULL,
		choice, press_buttons, press_mods, px, py, sigcalls, races,
		mtk_window_scale(win), mtk_window_focused(win), mtk_seat(m) != NULL,
		mtk_display(m) != NULL, mtk_serial(m) != 0, menu_configs, handles, text);
	if (!strcmp(now, last) ||
	    (size_t)snprintf(tmp, sizeof tmp, "%s.tmp", state_path) >= sizeof tmp)
		return;
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fputs(now, f);
	if (fclose(f) == 0 && rename(tmp, state_path) == 0)
		strcpy(last, now);
}

static void button_rect(int *x, int *y, int *w, int *h)
{
	*x = MTK_BORDER + PAD;
	*y = MTK_BORDER + PAD;
	*w = BTN_W;
	*h = barh;
}

static void field_rect(int ww, int wh, int *x, int *y, int *w, int *h)
{
	*x = MTK_BORDER + PAD;
	*y = MTK_BORDER + 2 * PAD + barh;
	*w = ww - 2 * (MTK_BORDER + PAD);
	*h = wh - *y - MTK_BORDER - PAD;
}

static int inside(double px, double py, int x, int y, int w, int h)
{
	return px >= x && py >= y && px < x + w && py < y + h;
}

static void paint(struct mtk_window *w, cairo_t *cr, int ww, int wh, void *data)
{
	const struct mtk_shade *face = &theme.inactive;
	mtk_fill(cr, face->bg, 0, 0, ww, wh);
	mtk_border(cr, ww, wh, focused ? &theme.active : &theme.inactive);
	int x, y, bw, bh;
	button_rect(&x, &y, &bw, &bh);
	mtk_shadow(cr, x, y, bw, bh, MTK_BEVEL, face, pressed);
	const char *label = "Press";
	mtk_text(cr, lo, label, face->fg, x + (bw - mtk_text_width(lo, label)) / 2,
		y + 5 + pressed);
	field_rect(ww, wh, &x, &y, &bw, &bh);
	mtk_shadow(cr, x, y, bw, bh, MTK_BEVEL, face, 1);
	mtk_fill(cr, 0xf6f6f6, x + MTK_BEVEL, y + MTK_BEVEL, bw - 2 * MTK_BEVEL,
		bh - 2 * MTK_BEVEL);
	cairo_save(cr);
	cairo_rectangle(cr, x + MTK_BEVEL, y + MTK_BEVEL, bw - 2 * MTK_BEVEL,
		bh - 2 * MTK_BEVEL);
	cairo_clip(cr);
	mtk_text(cr, lo, text, 0x000000, x + MTK_BEVEL + 4, y + MTK_BEVEL + 3);
	cairo_restore(cr);
}

/* ---- the menu ---- */

static void menu_close(void)
{
	if (menu)
		mtk_window_destroy(menu);
	menu = NULL;
	menu_hot = -1;
}

static void menu_paint(struct mtk_window *w, cairo_t *cr, int ww, int wh, void *data)
{
	static const char *const item[] = { "One", "Two" };
	mtk_fill(cr, theme.inactive.bg, 0, 0, ww, wh);
	mtk_shadow(cr, 0, 0, ww, wh, MTK_BEVEL, &theme.inactive, 0);
	for (int i = 0; i < 2; i++) {
		int y = MTK_BEVEL + i * ITEM_H;
		uint32_t fg = theme.inactive.fg;
		if (i == menu_hot) {
			mtk_fill(cr, theme.active.bg, MTK_BEVEL, y, ww - 2 * MTK_BEVEL, ITEM_H);
			fg = theme.active.fg;
		}
		mtk_text(cr, lo, item[i], fg, MTK_BEVEL + 8, y + 4);
	}
}

static int menu_item_at(double x, double y)
{
	int i = (int)(y - MTK_BEVEL) / ITEM_H;
	return x >= 0 && x < MENU_W && y >= MTK_BEVEL && i < 2 ? i : -1;
}

static void menu_take(int i)
{
	if (i < 0)
		return;
	choice = i + 1;
	menu_close();
}

static void menu_pointer(struct mtk_window *w, const struct mtk_pointer *p, void *data)
{
	int at = menu_item_at(p->x, p->y);
	if (p->kind == MTK_MOTION || p->kind == MTK_ENTER) {
		if (at != menu_hot) {
			menu_hot = at;
			mtk_window_redraw(w);
		}
	} else if (p->kind == MTK_RELEASE && p->button == BTN_LEFT) {
		menu_take(at);
	}
}

static void menu_key(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	if (k->sym == XKB_KEY_Escape) {
		menu_close();
	} else if (k->sym == XKB_KEY_Down || k->sym == XKB_KEY_Up) {
		menu_hot = k->sym == XKB_KEY_Down ? (menu_hot + 1) % 2 :
			menu_hot <= 0 ? 1 : menu_hot - 1;
		mtk_window_redraw(w);
	} else if (k->sym == XKB_KEY_Return) {
		menu_take(menu_hot);
	}
}

/* A click on another client: the compositor ends it. */
static void menu_done(struct mtk_window *w, void *data)
{
	menu_close();
}

/* MTKDEMO_DROP_MENU: gone before it is drawn, from inside mtk's configure. */
static void menu_configure(struct mtk_window *w, int width, int height, unsigned states,
	void *data)
{
	menu_configs++;
	if (drop_menu)
		menu_close();
}

static const struct mtk_window_listener menu_listener = {
	.paint = menu_paint,
	.pointer = menu_pointer,
	.key = menu_key,
	.close = menu_done,
	.configure = menu_configure,
};

static void focus(struct mtk_window *w, int in, void *data)
{
	focused = in;
	mtk_window_redraw(w);
}

static int key_repeats(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	return k->sym == XKB_KEY_BackSpace || (*k->utf8 && !(k->mods & MTK_MOD_CTRL));
}

static void key(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	keys++;
	repeats += k->repeat;
	if (k->sym == XKB_KEY_Escape ||
	    ((k->mods & MTK_MOD_CTRL) && (k->latin == XKB_KEY_q || k->latin == XKB_KEY_Q))) {
		done = 1;
		mtk_quit(m, 0);
		return;
	}
	if (k->sym == XKB_KEY_F2) {
		mtk_window_set_size(w, 360, 140);
		mtk_window_set_min_size(w, 250, 110);
		mtk_window_set_title(w, "mtkdemo 360");
		return;
	}
	size_t n = strlen(text);
	if (k->sym == XKB_KEY_BackSpace) {
		while (n > 0 && ((unsigned char)text[n - 1] & 0xc0) == 0x80)
			n--;    /* to the start of the character */
		if (n > 0)
			text[n - 1] = 0;
	} else if (*k->utf8 && !(k->mods & MTK_MOD_CTRL) &&
		   (unsigned char)k->utf8[0] >= 0x20 &&
		   n + strlen(k->utf8) < sizeof text) {
		strcat(text, k->utf8);
	}
	mtk_window_redraw(w);
}

static void pointer(struct mtk_window *w, const struct mtk_pointer *p, void *data)
{
	int ww, wh, x, y, bw, bh;
	mtk_window_size(w, &ww, &wh);
	switch (p->kind) {
	case MTK_ENTER:
	case MTK_MOTION:
		field_rect(ww, wh, &x, &y, &bw, &bh);
		mtk_window_set_cursor(w, inside(p->x, p->y, x, y, bw, bh) ? "text" : NULL);
		break;
	case MTK_PRESS:
		/* With the menu up a click on our own window is ours to close it. */
		if (menu) {
			menu_close();
			break;
		}
		button_rect(&x, &y, &bw, &bh);
		press_x = (int)p->x;
		press_y = (int)p->y;
		press_buttons = (int)p->buttons;
		press_mods = (int)p->mods;
		if (p->button == BTN_RIGHT) {
			menu = mtk_popup_new(w, &(struct mtk_popup_config){
				.x = press_x, .y = press_y, .width = MENU_W,
				.height = 2 * ITEM_H + 2 * MTK_BEVEL, .grab = 1,
				.serial = p->serial,
			}, &menu_listener, NULL);
			break;
		}
		if (p->button == BTN_LEFT && inside(p->x, p->y, x, y, bw, bh)) {
			pressed = 1;
			mtk_window_redraw(w);
		}
		break;
	case MTK_RELEASE:
		if (pressed) {
			button_rect(&x, &y, &bw, &bh);
			clicks += inside(p->x, p->y, x, y, bw, bh);
			pressed = 0;
			mtk_window_redraw(w);
		}
		break;
	case MTK_LEAVE:
		if (pressed) {
			pressed = 0;
			mtk_window_redraw(w);
		}
		break;
	case MTK_SCROLL:
		scrolls++;
		steps += p->steps_y;
		break;
	}
}

static void on_scale(struct mtk_window *w, int s, void *data)
{
	scale = s;
}

/* The compositor's close: the own loop does not hear mtk_quit(). */
static void on_close(struct mtk_window *w, void *data)
{
	done = 1;
	mtk_quit(m, 0);
}

static const struct mtk_window_listener listener = {
	.paint = paint,
	.focus = focus,
	.key = key,
	.key_repeats = key_repeats,
	.pointer = pointer,
	.scale = on_scale,
	.close = on_close,
};

static void on_signal(int sig)
{
	int saved = errno;
	if (write(sig_pipe[1], sig == SIGUSR2 ? "r" : "x", 1) < 0) {
		/* full: the count is a lower bound */
	}
	errno = saved;
}

/* Two timers due in the same round, each cancelling the other: one runs. */
static void race(struct mtk *mm, void *data)
{
	races++;
	mtk_timer_cancel(mm, data == &race_a ? race_b : race_a);
}

static void signal_read(struct mtk *mm, int fd, short revents, void *data)
{
	char buf[64];
	ssize_t n;
	sigcalls++;
	while ((n = read(fd, buf, sizeof buf)) > 0)
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == 'x') {
				signals++;
			} else if (buf[i] == 'r') {
				race_a = mtk_timer(mm, 0, race, &race_a);
				race_b = mtk_timer(mm, 0, race, &race_b);
				unwatch_extra = 1;
			}
		}
}

static void extra_read(struct mtk *mm, int fd, short revents, void *data)
{
}

static void tick(struct mtk *mm, void *data)
{
	ticks++;
	mtk_timer(mm, 100, tick, NULL);
}

static void idle(struct mtk *mm, void *data)
{
	/* After SIGUSR2: the second pipe goes, and the first has something to read
	 * in this round's poll, which must ask for it once. */
	if (unwatch_extra) {
		unwatch_extra = 0;
		mtk_unwatch_fd(mm, extra_pipe[0]);
		if (write(sig_pipe[1], "-", 1) < 0) {
			/* full: it is readable anyway */
		}
	}
	state_write();
}

/* The loop a client with another library's has: mtk's fd beside its own. */
static int own_loop(void)
{
	while (!done) {
		struct pollfd p[2];
		int timeout = -1;
		mtk_before_poll(m, &p[0], &timeout);
		p[1] = (struct pollfd){ .fd = sig_pipe[0], .events = POLLIN };
		if (poll(p, 2, timeout) < 0) {
			if (errno != EINTR) {
				perror("mtkdemo: poll");
				return 1;
			}
			p[0].revents = p[1].revents = 0;
		}
		mtk_after_poll(m, p[0].revents);
		if (p[1].revents)
			signal_read(m, sig_pipe[0], p[1].revents, NULL);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		fprintf(stderr, "usage: mtkdemo\n");
		return 1;
	}
	state_path = getenv("MTKDEMO_STATE");
	const char *e = getenv("MTKDEMO_FIXED");
	int fixed = e && *e == '1';
	e = getenv("MTKDEMO_WIDTH");
	int width = e && atoi(e) > 0 ? atoi(e) : 300;
	e = getenv("MTKDEMO_DECO");
	int deco = e && *e == '1';
	e = getenv("MTKDEMO_MAXIMIZED");
	int maximized = e && *e == '1';
	e = getenv("MTKDEMO_DROP_MENU");
	drop_menu = e && *e == '1';

	m = mtk_open(&(struct mtk_config){ .name = "mtkdemo", .idle = idle });
	if (!m)
		return 1;
	started = mtk_now();
	mtk_theme_load(&theme);
	PangoFontDescription *font = mtk_dock_font("mtkdemo");
	lo = mtk_layout_new(font);
	PangoContext *pctx = pango_layout_get_context(lo);
	PangoFontMetrics *fm = pango_context_get_metrics(pctx, font, NULL);
	barh = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(fm)) +
		PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(fm)) + 10;
	pango_font_metrics_unref(fm);
	pango_font_description_free(font);

	win = mtk_window_new(m, &(struct mtk_window_config){
		.width = width, .height = 120, .fixed = fixed, .min_width = 200,
		.min_height = 100, .server_decoration = deco,
		.maximized = maximized,
	}, &listener, NULL);

	if (pipe2(sig_pipe, O_NONBLOCK | O_CLOEXEC) < 0 ||
	    pipe2(extra_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		perror("mtkdemo: pipe");
		return 1;
	}
	signal(SIGUSR1, on_signal);
	signal(SIGUSR2, on_signal);
	/* The extra one first: unwatched, sig_pipe's watch moves into its place. */
	mtk_watch_fd(m, extra_pipe[0], POLLIN, extra_read, NULL);
	mtk_watch_fd(m, sig_pipe[0], POLLIN, signal_read, NULL);
	mtk_timer(m, 100, tick, NULL);
	e = getenv("MTKDEMO_OWN_LOOP");
	int status = e && *e == '1' ? own_loop() : mtk_run(m);

	state_write();
	g_object_unref(lo);
	mtk_close(m);
	close(sig_pipe[0]);
	close(sig_pipe[1]);
	close(extra_pipe[0]);
	close(extra_pipe[1]);
	return status;
}
