/* rc.c - resources from mwc's mwmrc, and the theme from them. */
#define _GNU_SOURCE
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mtk.h"

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

/* As mwc's add_rule(): "Mwm*Client*resource", '.' and '*' both separate, the
 * client is all between the first and the last part and may have dots of its own.
 * app NULL: only global lines. */
static int key_matches(const char *k, const char *app, const char *res)
{
	size_t prefix = strcspn(k, "*.");
	const char *r = k + strlen(k);
	while (r > k && r[-1] != '*' && r[-1] != '.')
		r--;
	if (prefix != 3 || !k[prefix] || strncasecmp(k, "mwm", 3) || !*r ||
	    strcasecmp(r, res))
		return 0;
	if (r - k == (ptrdiff_t)prefix + 1)
		return !app;
	size_t len = r - k - prefix - 2;
	const char *c = k + prefix + 1;
	return app && len > 0 && !memchr(c, '*', len) && strlen(app) == len &&
		!strncasecmp(c, app, len);
}

static char *lookup(const char *app, const char *key, char *val, size_t n)
{
	char path[PATH_MAX];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	int len;
	if (xdg && *xdg)
		len = snprintf(path, sizeof path, "%s/mwc/mwmrc", xdg);
	else if (home && *home)
		len = snprintf(path, sizeof path, "%s/.config/mwc/mwmrc", home);
	else
		return NULL;    /* as mwc: no HOME, no mwmrc, not /.config */
	if (len < 0 || (size_t)len >= sizeof path)
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *found = NULL, *line = NULL;       /* whole lines, however long */
	size_t cap = 0;
	while (getline(&line, &cap, f) > 0) {
		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		*colon = 0;
		if (!key_matches(trim(line), app, key))
			continue;
		char *v = trim(colon + 1);
		/* The last line wins, as in mwc: one too long to keep is no value,
		 * not a cut one and not the line before it. */
		found = NULL;
		size_t vl = strlen(v);
		if (vl < n) {
			memcpy(val, v, vl + 1);
			found = val;
		}
	}
	free(line);
	fclose(f);
	return found;
}

char *mtk_mwmrc_get(const char *key, char *val, size_t n)
{
	return lookup(NULL, key, val, n);
}

char *mtk_mwmrc_client_get(const char *app, const char *key, char *val, size_t n)
{
	return app && *app ? lookup(app, key, val, n) : NULL;
}

static int rc_color(const char *res, uint32_t *out)
{
	char v[256];
	return mtk_parse_color(mtk_mwmrc_get(res, v, sizeof v), out);
}

/* As mwc's setting_color(): the environment, the mwmrc, the default; *builtin says
 * the default it was. One that is no colour counts as not set. */
static uint32_t setting(const char *env, const char *res, uint32_t def, int *builtin)
{
	uint32_t rgb;
	*builtin = 0;
	if (mtk_parse_color(getenv(env), &rgb) || rc_color(res, &rgb))
		return rgb;
	*builtin = 1;
	return def;
}

void mtk_theme_load(struct mtk_theme *t)
{
	/* IRIX 6.5's Base scheme, as mwc's defaults: a grey, and a light stone for
	 * the window that has the focus. */
	int builtin, unused;
	uint32_t bg = setting("MWC_INACTIVE", "background", 0xaaaaaa, &builtin);
	uint32_t act = setting("MWC_ACTIVE", "activeBackground", 0xc6c1aa, &unused);
	uint32_t fg, afg;
	t->has_fg = rc_color("foreground", &fg);
	t->has_active_fg = rc_color("activeForeground", &afg);
	/* The built-in grey takes the Base palette's black text, as in mwc; Motif
	 * would pick white, at 67 %. A grey that is set, #aaaaaa too, gets Motif's
	 * choice. */
	t->inactive = mtk_shade(bg, t->has_fg ? fg : builtin ? 0x000000 :
		mtk_text_color(bg));
	t->active = mtk_shade(act, t->has_active_fg ? afg : mtk_text_color(act));
	t->dim = mtk_dimmed(bg, t->inactive.fg);
}
