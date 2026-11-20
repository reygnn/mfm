/* seats.c - the seats on offer, kept in the order they came */
#include "seats.h"

int seats_add(struct seats *s, unsigned name, unsigned ver)
{
	if (s->n == MAX_SEATS)
		return 0;
	s->name[s->n] = name;
	s->ver[s->n] = ver;
	s->n++;
	return 1;
}

/* The later ones move up, so the oldest stays first. */
void seats_remove(struct seats *s, unsigned name)
{
	for (int i = 0; i < s->n; i++)
		if (s->name[i] == name) {
			for (s->n--; i < s->n; i++) {
				s->name[i] = s->name[i + 1];
				s->ver[i] = s->ver[i + 1];
			}
			return;
		}
}

int seats_next(const struct seats *s, unsigned *name, unsigned *ver)
{
	if (!s->n)
		return 0;
	*name = s->name[0];
	*ver = s->ver[0];
	return 1;
}
