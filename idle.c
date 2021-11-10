/*-
 * Copyright (c) 2013, David Xu <davidxu@freebsd.org>
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

#include "opt_hwpmc_hooks.h"
#include "opt_kdtrace.h"
#include "opt_sched.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/sysctl.h>

#include "sched.h"

static void
idle_put_prev_thread(struct rq *rq, struct thread *td, int flags)
{
	TD_SET_CAN_RUN(td);
}

static struct thread *
idle_pick_next_thread(struct rq *rq)
{
	return (PCPU_GET(idlethread));
}

static void
idle_activate_thread(struct rq *rq, struct thread *td)
{
	TD_SET_RUNNING(td);
}

static void
idle_enqueue_thread(struct rq *rq, struct thread *td, int flags)
{
}

static void
idle_dequeue_thread(struct rq *rq, struct thread *td, int flags)
{
}

static void
idle_check_preempt_curr(struct rq *rq, struct thread *td)
{
}

static void
idle_change_priority(struct rq *rq, struct thread *td, u_char prio)
{
}

static void
idle_tick(struct rq *rq, struct thread *td)
{
}

static void
idle_fork_thread(struct rq *rq, struct thread *curr, struct thread *childtd)
{
}

static void
idle_switched_to(struct rq *rq, struct thread *td)
{
}

struct sched_class idle_class = {
	.next = NULL,
	.name = "idle",
	.put_prev_thread = idle_put_prev_thread,
	.pick_next_thread = idle_pick_next_thread,
	.activate_thread = idle_activate_thread,
	.sched_tick = idle_tick,
	.change_priority = idle_change_priority,
	.check_preempt_curr = idle_check_preempt_curr,
	.enqueue_thread = idle_enqueue_thread,
	.dequeue_thread = idle_dequeue_thread,
	.fork_thread = idle_fork_thread,
	.switched_from = NULL,
	.switched_to = idle_switched_to
};
