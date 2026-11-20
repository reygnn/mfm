/* seats.h - the seats on offer, which one to take: make check tests it */
#ifndef SEATS_H
#define SEATS_H

/* The seats the compositor has announced and not removed, oldest first, by their
 * name in the registry and version: one is bound, the others wait for it to go. */
#define MAX_SEATS 8
struct seats {
	unsigned name[MAX_SEATS], ver[MAX_SEATS];
	int n;
};

/* A seat announced; 0 if the list is full and it is not kept. */
int seats_add(struct seats *s, unsigned name, unsigned ver);

/* A global removed: dropped if it is a seat in the list. */
void seats_remove(struct seats *s, unsigned name);

/* The seat to take, the oldest left; 0 if there is none. */
int seats_next(const struct seats *s, unsigned *name, unsigned *ver);

#endif
