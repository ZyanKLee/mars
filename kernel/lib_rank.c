// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
// (c) 2012 Thomas Schoebel-Theuer

//#define BRICK_DEBUGGING
//#define XIO_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>

#include "xio_bricks/xio.h"
#include "lib_rank.h"

void ranking_compute(struct rank_data *rkd, const struct rank_info rki[], int x)
{
	int points = 0;
	int i;

	for (i = 0; ; i++) {
		int x0;
		int x1;
		int y0;
		int y1;

		x0 = rki[i].rki_x;
		if (x < x0)
			break;

		x1 = rki[i+1].rki_x;

		if (unlikely(x1 == RKI_DUMMY)) {
			points = rki[i].rki_y;
			break;
		}

		if (x > x1)
			continue;

		y0 = rki[i].rki_y;
		y1 = rki[i+1].rki_y;

		// linear interpolation
		points = ((long long)(x - x0) * (long long)(y1 - y0)) / (x1 - x0) + y0;
		break;
	}
	rkd->rkd_tmp += points;
}
EXPORT_SYMBOL_GPL(ranking_compute);

int ranking_select(struct rank_data rkd[], int rkd_count)
{
	int res = -1;
	long long max = LLONG_MIN / 2;
	int i;

	for (i = 0; i < rkd_count; i++) {
		struct rank_data *tmp = &rkd[i];
		long long rest = tmp->rkd_current_points;
		if (rest <= 0)
			continue;
		//rest -= tmp->rkd_got;
		if (rest > max) {
			max = rest;
			res = i;
		}
	}
	/* Prevent underflow in the long term
	 * and reset the "clocks" after each round of
	 * weighted round-robin selection.
	 */
	if (max < 0 && res >= 0) {
		for (i = 0; i < rkd_count; i++)
			rkd[i].rkd_got += max;
	}
	return res;
}
EXPORT_SYMBOL_GPL(ranking_select);
