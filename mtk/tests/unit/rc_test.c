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

/* fopen for rc.c, wrapped: the path it was asked for last. */
static char opened[256];
FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode)
{
	snprintf(opened, sizeof opened, "%s", path);
	return __real_fopen(path, mode);
}

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
	/* Without XDG_CONFIG_HOME it is ~/.config; without HOME there is none, an
	 * empty one is /.config as in mwc. */
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
	setenv("HOME", "", 1);
	opened[0] = 0;
	got(NULL, "background", NULL);
	CHECK(!strcmp(opened, "/.config/mwc/mwmrc"));
	unsetenv("HOME");
	unsetenv("XDG_CONFIG_HOME");
	opened[0] = 0;
	CHECK(got(NULL, "background", NULL));
	CHECK(!opened[0]);
}

TEST(theme)
{
	struct mtk_theme t;
	unsetenv("MWC_INACTIVE");
	unsetenv("MWC_ACTIVE");
	setenv("XDG_CONFIG_HOME", dir, 1);
	rc(dir, "");
	mtk_theme_load(&t);
	/* mwc's defaults, IRIX's Base scheme, with Motif's shadows */
	CHECK_INT(t.inactive.bg, 0xaaaaaa);
	CHECK_INT(t.inactive.top, 0xdadada);
	CHECK_INT(t.inactive.bot, 0x5a5a5a);
	CHECK_INT(t.active.bg, 0xc6c1aa);
	CHECK_INT(t.active.top, 0xe7e5db);
	CHECK_INT(t.active.bot, 0x6b685c);
	CHECK_INT(t.active.fg, 0x000000);       /* at 74 %, Motif's black */
	CHECK(!t.has_fg && !t.has_active_fg);
	/* The built-in grey takes black, where Motif gives white at 67 %. */
	CHECK_INT(t.inactive.fg, 0x000000);
	CHECK_INT(t.dim, 0x3c3c3c);
	/* A grey that is set, the same one too, gets Motif's white; so does one from
	 * the environment. One that is no colour counts as not set. */
	rc(dir, "Mwm*background: #aaaaaa\n");
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.fg, 0xffffff);
	rc(dir, "");
	setenv("MWC_INACTIVE", "#aaaaaa", 1);
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.fg, 0xffffff);
	unsetenv("MWC_INACTIVE");
	rc(dir, "Mwm*background: nonsense\n");
	mtk_theme_load(&t);
	CHECK(t.inactive.bg == 0xaaaaaa && t.inactive.fg == 0x000000);
	rc(dir, "Mwm*foreground: #ffffff\n");
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.fg, 0xffffff);
	rc(dir, "Mwm*activeBackground: #8c8c8c\n");
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.fg, 0x000000);

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

	/* As mwc hands a colour to Motif's rules: the 8 bits shown, each channel's
	 * 16 cut to the high byte. Rounded, or kept at 16, these would land on the
	 * other side of a threshold. */
	rc(dir, "Mwm*background: rgb:3380/3380/3380\n");     /* 33, dark below 20 % */
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.bg, 0x333333);
	CHECK(t.inactive.bot >> 16 < 0x33);             /* medium: darker */
	rc(dir, "Mwm*background: rgb:edf8/edf8/edf8\n");     /* ed, not above 93 % */
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.bg, 0xededed);
	CHECK(t.inactive.top >> 16 > 0xed);             /* medium: lighter */
	rc(dir, "Mwm*background: #b310b310b310\n");          /* b3, above 70 % */
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.bg, 0xb3b3b3);
	CHECK_INT(t.inactive.fg, 0x000000);
}

/* IRIX's client palette: basicBackground and textForeground for the tools, the frame
 * colours for mwc; the text has 4.5:1 where the palette or Motif's rule has less. */
TEST(palette)
{
	struct mtk_theme t;
	unsetenv("MWC_INACTIVE");
	unsetenv("MWC_ACTIVE");
	setenv("XDG_CONFIG_HOME", dir, 1);
	rc(dir, "Mwm*background: #8c8c8c\n");
	mtk_theme_load(&t);
	CHECK(!t.palette);

	/* Buckingham: Motif's white on the frames' greys has 3.4:1 and 3.0:1. */
	rc(dir, "Mwm*activeBackground: #9991a6\nMwm*background: #8c8c8c\n"
	   "Mwm*basicBackground: #795f78\nMwm*textForeground: #fffffe\n");
	mtk_theme_load(&t);
	CHECK(t.palette && !t.has_fg && !t.has_active_fg);
	CHECK_INT(t.inactive.bg, 0x795f78);
	CHECK_INT(t.inactive.fg, 0xfffffe);             /* 5.6:1, as the palette has it */
	CHECK_INT(t.inactive.top, mtk_shade(0x795f78, 0).top);
	CHECK_INT(t.inactive.bot, mtk_shade(0x795f78, 0).bot);
	CHECK_INT(t.active.bg, 0x9991a6);
	CHECK_INT(t.active.fg, 0x000000);               /* 7.0:1, Motif's white 3.0:1 */
	CHECK(mtk_contrast_ratio(t.inactive.bg, t.dim) >= 4.5f);
	CHECK_INT(t.dim, mtk_dimmed(0x795f78, 0xfffffe));       /* on the palette's face */

	/* Without textForeground black or white; foreground is the frames' alone. */
	rc(dir, "Mwm*basicBackground: #d9d9d9\nMwm*foreground: #ffffff\n");
	mtk_theme_load(&t);
	CHECK(t.palette && !t.has_fg);
	CHECK_INT(t.inactive.fg, 0x000000);
	/* ... the better of them, where Motif's rule would take white at 3.4:1. */
	rc(dir, "Mwm*basicBackground: #8c8c8c\n");
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.fg, 0x000000);

	/* Arizona's palette text has 3.8:1: moved as far as 4.5:1 needs. */
	rc(dir, "Mwm*basicBackground: #868373\nMwm*textForeground: #ffffff\n");
	mtk_theme_load(&t);
	CHECK(t.inactive.fg != 0xffffff);
	CHECK(mtk_contrast_ratio(0x868373, t.inactive.fg) >= 4.5f);

	/* activeForeground is kept if it reads, else moved: Monet's white on #aaa5d5. */
	rc(dir, "Mwm*basicBackground: #b688a5\nMwm*activeBackground: #aaa5d5\n"
	   "Mwm*activeForeground: #ffffff\n");
	mtk_theme_load(&t);
	CHECK(t.has_active_fg);
	CHECK(mtk_contrast_ratio(0xaaa5d5, t.active.fg) >= 4.5f);
	rc(dir, "Mwm*basicBackground: #b688a5\nMwm*activeBackground: #aaa5d5\n"
	   "Mwm*activeForeground: #000033\n");
	mtk_theme_load(&t);
	CHECK_INT(t.active.fg, 0x000033);

	/* One that is no colour counts as not set, as everywhere. */
	rc(dir, "Mwm*basicBackground: nonsense\nMwm*background: #102030\n");
	mtk_theme_load(&t);
	CHECK(!t.palette);
	CHECK_INT(t.inactive.bg, 0x102030);
	rc(dir, "Mwm*basicBackground: #21628c\nMwm*textForeground: nonsense\n");
	mtk_theme_load(&t);
	CHECK_INT(t.inactive.fg, 0xffffff);
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
	RUN(palette);
	char cmd[64];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	if (system(cmd) != 0)
		fprintf(stderr, "could not remove %s\n", dir);
	return unit_done();
}
