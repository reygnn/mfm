/* rc_test.c - rc.c: the mwmrc as mwc reads it, and the theme from it. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../mtk.h"
#include "unit.h"

static char dir[] = "/tmp/mtk_rc_test.XXXXXX";

/* text as base/mwc/mwmrc */
static void rc(const char *base, const char *text)
{
	char path[256];
	snprintf(path, sizeof path, "%s/mwc", base);
	mkdir(path, 0700);
	snprintf(path, sizeof path, "%s/mwc/mwmrc", base);
	FILE *f = fopen(path, "w");
	if (!f || fputs(text, f) == EOF || fclose(f)) {
		perror(path);
		exit(1);
	}
}

static int got(const char *app, const char *key, const char *want)
{
	char v[16];
	const char *r = app ? mtk_mwmrc_client_get(app, key, v, sizeof v) :
		mtk_mwmrc_get(key, v, sizeof v);
	return want ? r == v && !strcmp(v, want) : !r;
}

TEST(global)
{
	setenv("XDG_CONFIG_HOME", dir, 1);
	CHECK(got(NULL, "background", NULL));   /* no file */
	rc(dir, "Mwm*background: #111111\n"
	   "  mwm.BACKGROUND \t:  #222222  \n"  /* any case, "." or "*", blanks */
	   "Mwm*backgroundX: #333333\n"
	   "Mwm*foo*background: #444444\n"      /* not global */
	   "Xwm*background: #555555\n"
	   "Mwm*background #666666\n"           /* no colon */
	   "Mwm*foreground:\n"
	   "Mwm*activeBackground: 0123456789abcdef\n"
	   "Mwm*long: 0123456789abcdef\n"
	   "Mwm*long: short\n"
	   "Mwm*short: short\n"
	   "Mwm*short: 0123456789abcdef\n");
	CHECK(got(NULL, "background", "#222222"));      /* the last line wins */
	CHECK(got(NULL, "Background", "#222222"));
	CHECK(got(NULL, "foreground", ""));
	CHECK(got(NULL, "activeBackground", NULL));     /* too long: none, not cut */
	CHECK(got(NULL, "long", "short"));
	CHECK(got(NULL, "short", NULL));                /* not the one before either */
	CHECK(got(NULL, "backgroun", NULL));
}

TEST(client)
{
	setenv("XDG_CONFIG_HOME", dir, 1);
	rc(dir, "Mwm*background: #111111\n"
	   "Mwm*digitalClock*dock: true\n"
	   "Mwm.DIGITALCLOCK.Dock: yes\n"       /* the last, in any case */
	   "Mwm*org.gnome.Terminal*dock: x\n"   /* a client may have dots */
	   "Mwm**dock: empty\n"                 /* names none */
	   "Mwm*XTerm*vt100*dock: two\n");      /* nor a client of two parts */
	CHECK(got("digitalClock", "dock", "yes"));
	CHECK(got("digitalclock", "DOCK", "yes"));
	CHECK(got("org.gnome.Terminal", "dock", "x"));
	CHECK(got("digital", "dock", NULL));
	CHECK(got("digitalClock", "background", NULL)); /* not the global line */
	CHECK(got(NULL, "dock", NULL));
	CHECK(got("", "dock", NULL));
	CHECK(got("XTerm", "dock", NULL));
	CHECK(got("XTerm*vt100", "dock", NULL));
}

TEST(long_line)
{
	/* A line longer than any buffer is read whole: setting-like pieces 32 bytes
	 * apart, so that a reader cutting it into pieces of most sizes gets one that
	 * starts with "Mwm*background:". */
	enum { UNIT = 32, UNITS = 300 };
	char *big = malloc(UNIT * (UNITS + 2));
	int at = sprintf(big, "Mwm*background: #777777\n");
	at += sprintf(big + at, "%-*s", UNIT, "!");
	for (int i = 0; i < UNITS; i++)
		at += sprintf(big + at, "%-*s", UNIT, "Mwm*background: #999999");
	strcpy(big + at, "\n");
	setenv("XDG_CONFIG_HOME", dir, 1);
	rc(dir, big);
	free(big);
	CHECK(got(NULL, "background", "#777777"));
}

TEST(where)
{
	/* Without XDG_CONFIG_HOME it is ~/.config; without HOME there is none. */
	unsetenv("XDG_CONFIG_HOME");
	setenv("HOME", "/nonexistent", 1);
	CHECK(got(NULL, "background", NULL));
	char home[64], cfg[128];
	snprintf(home, sizeof home, "%s/home", dir);
	snprintf(cfg, sizeof cfg, "%s/.config", home);
	mkdir(home, 0700);
	mkdir(cfg, 0700);
	rc(cfg, "Mwm*background: #888888\n");
	setenv("HOME", home, 1);
	CHECK(got(NULL, "background", "#888888"));
	setenv("XDG_CONFIG_HOME", "", 1);       /* empty is unset */
	CHECK(got(NULL, "background", "#888888"));
	unsetenv("HOME");
	unsetenv("XDG_CONFIG_HOME");
	CHECK(got(NULL, "background", NULL));
}

TEST(theme)
{
	struct mtk_theme t;
	unsetenv("MWC_INACTIVE");
	unsetenv("MWC_ACTIVE");
	setenv("XDG_CONFIG_HOME", dir, 1);
	rc(dir, "");
	mtk_theme_load(&t);
	/* Motif's defaults, as mwc's */
	CHECK_INT(t.inactive.bg, 0xaeb2c3);
	CHECK_INT(t.inactive.top, 0xdddee6);
	CHECK_INT(t.inactive.bot, 0x5e6069);
	CHECK_INT(t.inactive.fg, 0x000000);
	CHECK_INT(t.active.bg, 0x7387ad);
	CHECK_INT(t.active.fg, 0xffffff);
	CHECK(!t.has_fg && !t.has_active_fg);
	CHECK_INT(t.dim, 0x3d3e44);

	rc(dir, "Mwm*background: #102030\nMwm*activeBackground: gold\n"
	   "Mwm*foreground: white\nMwm*activeForeground: rgb:1/2/3\n");
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.bg, 0x102030);
	CHECK_INT(t.active.bg, 0xffd700);
	CHECK(t.has_fg && t.has_active_fg);
	CHECK_INT(t.inactive.fg, 0xffffff);
	CHECK_INT(t.active.fg, 0x112233);
	CHECK_INT(t.active.top, mtk_shade(0xffd700, 0).top);

	/* Text as Motif picks it, where WCAG would take black. */
	rc(dir, "Mwm*background: #7387ad\n");
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.fg, 0xffffff);

	/* The environment first; one that is no colour is passed over. */
	setenv("MWC_ACTIVE", "#b24d7a", 1);
	setenv("MWC_INACTIVE", "no colour", 1);
	rc(dir, "Mwm*background: #102030\nMwm*foreground: nonsense\n");
	mtk_theme_load(&t);
	CHECK_INT(t.active.bg, 0xb24d7a);
	CHECK_INT(t.inactive.bg, 0x102030);
	CHECK(!t.has_fg);
	CHECK_INT(t.inactive.fg, 0xffffff);
	unsetenv("MWC_ACTIVE");
	unsetenv("MWC_INACTIVE");
}

int main(void)
{
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	RUN(global);
	RUN(client);
	RUN(long_line);
	RUN(where);
	RUN(theme);
	char cmd[64];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	if (system(cmd) != 0)
		fprintf(stderr, "could not remove %s\n", dir);
	return unit_done();
}
