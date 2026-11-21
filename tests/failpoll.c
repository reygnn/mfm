/* failpoll.c - LD_PRELOAD for black-box tests: the poll of an mtk loop with one fd
 * watched beside Wayland's, as mtkdemo's or osDisplay's, fails as under memory
 * pressure. libwayland's own polls have one fd and go through. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>

int poll(struct pollfd *fds, nfds_t n, int timeout)
{
	static int (*real)(struct pollfd *, nfds_t, int);
	if (n == 2) {
		errno = ENOMEM;
		return -1;
	}
	if (!real)
		real = (int (*)(struct pollfd *, nfds_t, int))dlsym(RTLD_NEXT, "poll");
	return real(fds, n, timeout);
}

/* With _FORTIFY_SOURCE a poll of a count not known at compile time, as mtk's, goes
 * through here. */
int __poll_chk(struct pollfd *fds, nfds_t n, int timeout, size_t len)
{
	(void)len;
	return poll(fds, n, timeout);
}
