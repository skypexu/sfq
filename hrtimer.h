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

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/pcpu.h>
#include <sys/time.h>
#include <sys/syscallsubr.h>

enum hrtimer_mode {
	HRTIMER_MODE_REL,
	HRTIMER_MODE_ABS
};

enum hrtimer_restart {
	HRTIMER_NORESTART,	/* Timer is not restarted */
	HRTIMER_RESTART		/* Timer must be restarted */
};

struct hrtimer {
	struct callout		callout;
	struct timespec		expiry;
	enum hrtimer_restart	(*function)(struct hrtimer *);
	struct mtx		*lock;
};

void	hrtimer_init(struct hrtimer *, struct mtx *lock);
int	hrtimer_forward(struct hrtimer *, const struct timespec *,
		const struct timespec *);
int	hrtimer_start(struct hrtimer *, const struct timespec *,
		const enum hrtimer_mode);

static inline void
hrtimer_cb_get_time(struct hrtimer *timer, struct timespec *now)
{
	nanouptime(now);
}

static inline int
hrtimer_active(struct hrtimer *timer)
{
	return callout_active(&timer->callout);
}

static inline void
hrtimer_cancel(struct hrtimer *timer)
{
	callout_drain(&timer->callout);
}

static inline void
hrtimer_get_expiry(struct hrtimer *timer, struct timespec *ts)
{
	timer->expiry = *ts;
}
