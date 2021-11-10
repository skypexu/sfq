/*
 * Author: davidxu@freebsd.org
 *
 * http://en.wikipedia.org/wiki/Exponential_decay
 * http://en.wikipedia.org/wiki/Geometric_progression
 * http://baike.baidu.com/view/1149632.htm
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <err.h>

double _gamma;
double decay_factor;
double limit_factor;

void init(int halflife_time)
{
	_gamma = log(2) / halflife_time;
	decay_factor = exp(-1 * _gamma);

	/*
	 * The sum of  geometric sequence
	 * expression: a0 + a0 * r + a0 * r^2 ...
	 * result    : a0 * 1 / (1 - r)
	 */
	limit_factor = 1 / (1 - decay_factor);
}

double decay(double val /* , int delta */)
{
	val *= decay_factor;
	return (val);
}

int main(int argc, char **argv)
{
	double val = 1024;
	double r = 1;
	int n, i;

	if (argc < 2)
		errx(1, "please provide decay count for half-life");

	n = atoi(argv[1]);

	init(n);

	for (i = 0; i < n; ++i) {
		printf("/* %2d */ %0llx,\n", i,
			(unsigned long long) (((1ULL << 32)-1)  * r));
		r *= decay_factor;
	}

	printf("max possible value: %f\n", limit_factor * 1024);

	for (i = 0; i < n; ++i) {
		val = decay(val);
		printf("/* %2d */ %d,\n", i + 1, (int) val);
		val += 1024;
	}
	return (0);
}
