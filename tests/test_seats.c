/* test_seats.c - the seat list: make check */
#include <stdio.h>

#include "seats.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int next_is(const struct seats *s, unsigned name, unsigned ver)
{
	unsigned n = 0, v = 0;
	return seats_next(s, &n, &v) && n == name && v == ver;
}

int main(void)
{
	struct seats s = { 0 };
	unsigned n, v;
	CHECK(!seats_next(&s, &n, &v));
	/* seat0 and seat1 at the start: seat0 is taken */
	CHECK(seats_add(&s, 10, 7) && seats_add(&s, 11, 5));
	CHECK(next_is(&s, 10, 7));
	/* other globals going leave the list alone */
	seats_remove(&s, 99);
	CHECK(s.n == 2 && next_is(&s, 10, 7));
	/* seat0 goes: seat1, there all along, is next */
	seats_remove(&s, 10);
	CHECK(next_is(&s, 11, 5));
	/* a new one comes after it */
	CHECK(seats_add(&s, 12, 9));
	CHECK(next_is(&s, 11, 5));
	/* one that is not taken goes: the taken one stays first */
	seats_remove(&s, 12);
	CHECK(s.n == 1 && next_is(&s, 11, 5));
	seats_remove(&s, 11);
	CHECK(s.n == 0 && !seats_next(&s, &n, &v));
	/* the order holds when one goes from the middle */
	for (unsigned i = 0; i < 3; i++)
		seats_add(&s, 20 + i, i);
	seats_remove(&s, 21);
	seats_remove(&s, 20);
	CHECK(s.n == 1 && next_is(&s, 22, 2));
	seats_remove(&s, 22);
	/* full: one more is not kept, the others are */
	for (unsigned i = 0; i < MAX_SEATS; i++)
		CHECK(seats_add(&s, 30 + i, 1));
	CHECK(!seats_add(&s, 99, 1) && s.n == MAX_SEATS);
	seats_remove(&s, 99);
	CHECK(s.n == MAX_SEATS);
	for (unsigned i = 0; i < MAX_SEATS; i++) {
		CHECK(next_is(&s, 30 + i, 1));
		seats_remove(&s, 30 + i);
	}
	CHECK(s.n == 0);
	printf("test_seats: %d/%d\n", checks - failures, checks);
	return failures != 0;
}
