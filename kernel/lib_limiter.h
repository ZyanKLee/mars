// (c) 2012 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef MARS_LIB_LIMITER_H
#define MARS_LIB_LIMITER_H

#include "brick.h"

#include <linux/utsname.h>

struct xio_limiter {
	/* hierarchy tree */
	struct xio_limiter *lim_father;
	/* tunables */
	int lim_max_rate;
	int lim_max_delay;
	int lim_min_window;
	int lim_max_window;
	/* readable */
	int lim_rate;
	int lim_cumul;
	int lim_count;
	long long lim_stamp;
	/* internal */
	long long lim_accu;
};

extern int xio_limit(struct xio_limiter *lim, int amount);

extern void xio_limit_sleep(struct xio_limiter *lim, int amount);

#endif
