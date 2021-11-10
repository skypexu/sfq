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

#ifdef SMP
#error "SMP is not supported yet"
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/sysctl.h>

#include "sched.h"


static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun);

struct rt_bandwidth def_rt_bandwidth;

static enum hrtimer_restart
sched_rt_period_timer(struct hrtimer *timer)
{
	struct rt_bandwidth *rt_b =
		__containerof(timer, struct rt_bandwidth, rt_period_timer);
	struct timespec now;
	int overrun;
	int idle = 0;

	for (;;) {
		mtx_lock_spin(&rt_b->rt_runtime_lock);
		hrtimer_cb_get_time(timer, &now);
		overrun = hrtimer_forward(timer, &now, &rt_b->rt_period);
		mtx_unlock_spin(&rt_b->rt_runtime_lock);
		if (!overrun)
			break;
		idle = do_sched_rt_period_timer(rt_b, overrun);
	}

	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

void
init_rt_bandwidth(struct rt_bandwidth *rt_b, uint64_t period, uint64_t runtime)
{
	ns_to_timespec(&rt_b->rt_period, period);
	rt_b->rt_runtime = runtime;
	mtx_init(&rt_b->rt_runtime_lock, "rt bandwidth", NULL, MTX_SPIN);
	hrtimer_init(&rt_b->rt_period_timer, &rt_b->rt_runtime_lock);
	rt_b->rt_period_timer.function = sched_rt_period_timer;
}

void
start_rt_bandwidth(struct rt_bandwidth *rt_b);

void
start_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	if (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF)
		return;

	if (hrtimer_active(&rt_b->rt_period_timer))
		return;

	mtx_lock_spin(&rt_b->rt_runtime_lock);
	start_bandwidth_timer(&rt_b->rt_period_timer, &rt_b->rt_period);
	mtx_unlock_spin(&rt_b->rt_runtime_lock);
}

static int do_sched_rt_period_timer(struct rt_bandwidth *rt_b, int overrun)
{
	return (0);
}

void
init_rt_rq(struct rt_rq *rt_rq, struct rq *rq)
{
	struct rt_prio_array *array;
	int i;
 
	array = &rt_rq->active;
	BIT_ZERO(NUM_RT_PRIO+1, &array->bitmap);
	for (i = 0; i < NUM_RT_PRIO; i++)
		TAILQ_INIT(&array->queue[i]);
	BIT_SET(NUM_RT_PRIO+1, NUM_RT_PRIO, &array->bitmap);
	rt_rq->rt_nr_running = 0;
#ifdef SMP
	rt_rq->highest_prio.curr = NUM_RT_PRIO;
	rt_rq->highest_prio.next = NUM_RT_PRIO;
	rt_rq->rt_nr_migratory = 0;
	rt_rq->overloaded = 0;
	prio_list_init(&rt_rq->pushable_tasks);
#endif
    
	rt_rq->rt_time = 0;
	rt_rq->rt_throttled = 0;
	rt_rq->rt_runtime = 0;
	mtx_init(&rt_rq->rt_runtime_lock, "rt runtime", NULL, MTX_SPIN);
}

#ifdef CONFIG_RT_GROUP_SCHED
static void
destroy_rt_bandwidth(struct rt_bandwidth *rt_b)
{
	hrtimer_cancel(&rt_b->rt_period_timer);
}

static struct rq *
rt_rq_to_rq(struct rt_rq *rt_rq)
{
	return (rt_rq->rq);
}

static struct rt_rq *
rt_se_to_rt_rq(struct sched_rt_entity *rt_se)
{
	return (rt_se->rt_rq);
}

void
rt_free_sched_group(struct sched_group *sg)
{
	int i;

	if (sg->rt_se)
		destroy_rt_bandwidth(&sg->rt_bandwidth);

	CPU_FOREACH(i) {
		if (sg->rt_rq)
			free(sg->rt_rq[i], M_SCHED);
		if (sg->rt_se)
			free(sg->rt_se[i], M_SCHED);
        }

	free(tg->rt_rq, M_SCHED);
	free(tg->rt_se, M_SCHED);
}

void
rt_init_group(struct sched_group *sg, struct rt_rq *rt_rq,
		struct sched_rt_entity *rt_se, int cpu,
		struct sched_rt_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	rt_rq->highest_prio.curr = MAX_RT_PRI;
	rt_rq->rt_nr_boosted = 0;
	rt_rq->rq = rq;
	rt_rq->sg = sg;

	sg->rt_rq[cpu] = rt_rq;
	sg->rt_se[cpu] = rt_se;

	if (!rt_se)
		return;

	if (!parent)
		rt_se->rt_rq = &rq->rt;
	else
		rt_se->rt_rq = parent->my_q;

	rt_se->my_q = rt_rq;
	rt_se->parent = parent;
}

int
rt_alloc_sched_group(struct sched_group *sg, struct sched_group *parent)
{
	struct rt_rq *rt_rq;
	struct sched_rt_entity *rt_se;
	int i;

	sg->rt_rq = malloc(sizeof(rt_rq) * mp_ncpus, M_SCHED);
	if (!sg->rt_rq)
		goto err;
	sg->rt_se = malloc(sizeof(rt_se) * mp_ncpus, M_SCHED);
	if (!sg->rt_se)
		goto err;

	init_rt_bandwidth(&sg->rt_bandwidth,
			timespec_to_ns(def_rt_bandwidth.rt_period), 0);

	CPU_FOREACH(i) {
		rt_rq = malloc(sizeof(struct rt_rq), M_SCHED);
		if (!rt_rq)
			goto err;

		rt_se = malloc(sizeof(struct sched_rt_entity), M_SCHED);
		if (!rt_se)
			goto err_free_rq;

		init_rt_rq(rt_rq, cpu_rq(i));
		rt_rq->rt_runtime = sg->rt_bandwidth.rt_runtime;
		rt_init_rt_group(sg, rt_rq, rt_se, i, parent->rt_se[i]);
	}

	return 1;

err_free_rq:
	free(rt_rq, M_SCHED);
err:
	return 0;
}

#else /* RT_GROUP_SCHED */

static inline struct rq *
rt_rq_to_rq(struct rt_rq *rt_rq)
{
	return __containerof(rt_rq, struct rq, rt);
}

static inline struct rt_rq *
rt_rq_of_se(struct sched_rt_entity *rt_se)
{
	struct thread *td = rt_se_to_td(rt_se);
	struct rq *rq = thread_rq(td);

	return (&rq->rt);
}

void
rt_free_sched_group(struct sched_group *sg)
{
}

int
rt_alloc_sched_group(struct sched_group *sg, struct sched_group *parent)
{
	return (1);
}

#endif

static void
rt_put_prev_thread(struct rq *rq, struct thread *td, int flags)
{
	struct td_sched *ts;

	RQ_LOCK_ASSERT(rq, MA_OWNED);
	ts = td->td_sched;
	if (ts->se.joined) {
		KASSERT((ts->se.flags & SEF_ONRUNQ) == 0, ("already on runq"));
		TD_SET_RUNQ(td);
		runq_add(&rq->realtime, td, 
		    ((flags & SW_TYPE_MASK) == SWT_PREEMPT) ? SRQ_PREEMPTED : 0);
		ts->se.flags |= SEF_ONRUNQ;
	}
	rq->rt_curr = NULL;
}

static struct thread *
rt_pick_next_thread(struct rq *rq)
{
	struct thread *td;

	RQ_LOCK_ASSERT(rq, MA_OWNED);
	td = runq_choose(&rq->realtime);
	return (td);
}

static void
rt_activate_thread(struct rq *rq, struct thread *td)
{
	struct sched_entity *se;

	se = td_to_se(td);
	MPASS(se->joined != 0);
	MPASS((se->flags & SEF_ONRUNQ) != 0);
	runq_remove(&rq->realtime, td);
	se->flags &= ~SEF_ONRUNQ;
	KASSERT((se->flags & SEF_ONSFQ) == 0, ("on sfq"));
	se->timing_start = sched_time_cpu(rq_cpu(rq));
	se->prev_sum_exec_time = se->sum_exec_time;
	rq->rt_curr = se;
	TD_SET_RUNNING(td);
}

static void
rt_enqueue_thread(struct rq *rq, struct thread *td, int flags)
{
	struct sched_entity *se;

	se = td_to_se(td);

	MPASS(se->joined == 0);
	KASSERT((se->flags & SEF_ONRUNQ) == 0, ("already on runq"));

	runq_add(&rq->realtime, td, (flags & ENQUEUE_HEAD) ? SRQ_PREEMPTED : 0);
	se->flags |= SEF_ONRUNQ;
	TD_SET_RUNQ(td);
	rq_run_add(rq, td);
	se->joined = 1;
	KASSERT((se->flags & SEF_ONSFQ) == 0, ("on sfq"));
}

static void
rt_dequeue_thread(struct rq *rq, struct thread *td, int flags)
{
	struct sched_entity *se;

	se = td_to_se(td);
	if (se->joined == 0)
		return;
	if (se->flags & SEF_ONRUNQ) {
		runq_remove(&rq->realtime, td);
		se->flags &= ~SEF_ONRUNQ;
	}
	rq_run_rem(rq, td);
	se->joined = 0;
	KASSERT((se->flags & SEF_ONSFQ) == 0, ("on sfq"));
}

static void
rt_check_preempt_curr(struct rq *rq, struct thread *td)
{
#if 0
	check_priority_preempt(td);
#endif
}

static void
rt_change_priority(struct rq *rq, struct thread *td, u_char prio)
{
	struct sched_entity *se;

	if (TD_ON_RUNQ(td) && prio < td->td_priority) {
		se = td_to_se(td);
		KASSERT((se->flags & SEF_ONRUNQ), ("not on runq"));
		runq_remove(&rq->realtime, td);
		td->td_priority = prio;
		runq_add(&rq->realtime, td, 0);
		check_priority_preempt(rq, td);
		return;
	}
	td->td_priority = prio;
}

static void
rt_tick(struct rq *rq, struct thread *td)
{
	unsigned long slice = rq_slice_ns(rq);
	unsigned long delta_exec;
	struct sched_entity *se;

	RQ_LOCK_ASSERT(rq, MA_OWNED);
	if (td->td_pri_class == PRI_ITHD)
		return;
	if (td->td_pri_class & PRI_FIFO_BIT)
                return;
	se = td_to_se(td);
	delta_exec = sched_time_cpu(rq_cpu(rq)) - se->timing_start;
	if (delta_exec >= slice)
		resched_thread(td);
}

static void
rt_fork_thread(struct rq *rq, struct thread *curr, struct thread *childtd)
{
}

static void
rt_switched_to(struct rq *rq, struct thread *td)
{
}

static void
rt_init_rq(struct rq *rq)
{
	runq_init(&rq->realtime);
}

struct sched_class realtime_class = {
	.next = &pss_class,
	.name = "realtime",
	.init_rq = rt_init_rq,
	.put_prev_thread = rt_put_prev_thread,
	.pick_next_thread = rt_pick_next_thread,
	.activate_thread = rt_activate_thread,
	.sched_tick = rt_tick,
	.change_priority = rt_change_priority,
	.check_preempt_curr = rt_check_preempt_curr,
	.enqueue_thread = rt_enqueue_thread,
	.dequeue_thread = rt_dequeue_thread,
	.fork_thread = rt_fork_thread,
	.switched_from = NULL,
	.switched_to = rt_switched_to
};
