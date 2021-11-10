/*-
 * Copyright (c) 2014, David Xu <davidxu@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_sched.h"
#include "sched.h"

void
hrtimer_init(struct hrtimer *timer, struct mtx *lock)
{
	timer->expiry.tv_sec = 0;
	timer->expiry.tv_nsec = 0;
	timer->function = NULL;
	timer->lock = lock;
	callout_init(&timer->callout, CALLOUT_MPSAFE);
}

static void
timeout_handler(void *arg)
{
	struct hrtimer *timer = arg;
	sbintime_t sbt;
	enum hrtimer_restart result;

	callout_deactivate(&timer->callout);

	result = timer->function(timer);
	if (result == HRTIMER_NORESTART)
		return;
	mtx_lock_spin(timer->lock);
	if (callout_active(&timer->callout)) {
		mtx_unlock_spin(timer->lock);
		return;
	}
	sbt = tstosbt(timer->expiry);
	callout_reset_sbt_on(&timer->callout, sbt, SBT_1NS,
		timeout_handler, timer, PCPU_GET(cpuid),
		C_DIRECT_EXEC | C_ABSOLUTE);
	mtx_unlock_spin(timer->lock);
}

/*
 * Start an timer on the current cpu.
 * return: 0 on success 1 when timer was active.
 */
int
hrtimer_start(struct hrtimer *timer, const struct timespec *ts,
	const enum hrtimer_mode mode)
{
	sbintime_t sbt;

	mtx_assert(timer->lock, MA_OWNED);

	if (mode == HRTIMER_MODE_ABS) {
		timer->expiry = *ts;
	} else {
		nanouptime(&timer->expiry);
		timespecadd(&timer->expiry, ts);
	}
	sbt = tstosbt(timer->expiry);
	callout_reset_sbt_on(&timer->callout, sbt, SBT_1NS,
		timeout_handler, timer, PCPU_GET(cpuid),
		C_DIRECT_EXEC | C_ABSOLUTE);
	return (0);
}

static inline void
timespec_add_ns(struct timespec *ts, uint64_t ns)
{
	struct timespec tmp;

	ns_to_timespec(&tmp, ns);
	timespecadd(ts, &tmp);
}

static inline uint64_t
timespec_div_ns(struct timespec *ts, uint64_t ns)
{
	return timespec_to_ns(ts) / ns;
}

/*
 * Forward the timer expiry so it will expire in the future.
 * Returns the number of overruns. 
 */
int
hrtimer_forward(struct hrtimer *timer, const struct timespec *now,
	const struct timespec *interval)
{
	struct timespec delta;
	uint64_t overrun = 1;

	mtx_assert(timer->lock, MA_OWNED);

	delta = *now;
	timespecsub(&delta, &timer->expiry);
	if (delta.tv_sec < 0)
		return (0);
	if (timespeccmp(&delta, interval, >=)) {
		int64_t incr = timespec_to_ns(interval);
		overrun += timespec_div_ns(&delta, incr);
		timespec_add_ns(&timer->expiry, overrun * incr);
	}
	timespecadd(&timer->expiry, interval);
	return (overrun);
}
