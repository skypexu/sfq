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

/*
 * This file implements the SFQ scheduler, a proportional share cpu scheduler.
 * SFQ is based on Start-time Fair Queueing.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_hwpmc_hooks.h"
#include "opt_kdtrace.h"
#include "opt_sched.h"

#ifdef SMP
#error "SMP is not supported yet"
#endif

#include "sched.h"
 
#define SCHED_SCALING_NONE	0
#define SCHED_SCALING_LINEAR	1
#define SCHED_SCALING_LOG	2

RB_PROTOTYPE(TIMELINE, sched_entity, tlink,
	int (*)(struct sched_entity *, struct sched_entity *));

static unsigned long sched_latency = 6000000UL;

static unsigned long sched_min_granularity = 750000UL;

/* must be the result of (latency div min_granularity) */
static unsigned int sched_nr_latency = 8;

static unsigned long wakeup_granularity = 650000UL;

static int gentle_sleeper = 0;

static int start_debit = 1;

static int child_runs_first = 0;

static int sched_scaling = SCHED_SCALING_LOG;

static int sched_last = 1;

static unsigned long sched_loadavg_period = 10000000;

#ifdef SFQ_GROUP_SCHED
static struct mtx shares_lock;
#endif

#define NICE_0_WEIGHT	1024
#define NICE_0_WMULT	4194304

#define IDLEPRIO_WEIGHT	3
#define IDLEPRIO_WMULT	1431655765

static const int prio_to_weight[41] = {
/*  -20 */        88818,        71054,        56843,        45475,        36380,
/*  -15 */        29104,        23283,        18626,        14901,        11921,
/*  -10 */         9537,         7629,         6104,         4883,         3906,
/*   -5 */         3125,         2500,         2000,         1600,         1280,
/*    0 */         1024,
/*    1 */          819,          655,          524,          419,          336,
/*    6 */          268,          215,          172,          137,          110,
/*   11 */           88,           70,           56,           45,           36,
/*   16 */           29,           23,           18,           15,           12,
};

static const int prio_to_wmult[41] = {
/*  -20 */        48356,        60446,        75558,        94446,       118058,
/*  -15 */       147573,       184467,       230589,       288233,       360285,
/*  -10 */       450347,       562979,       703631,       879575,      1099582,
/*   -5 */      1374389,      1717986,      2147483,      2684354,      3355443,
/*    0 */      4194304,
/*    1 */      5244160,      6557201,      8196502,     10250518,     12782640,
/*    6 */     16025997,     19976592,     24970740,     31350126,     39045157,
/*   11 */     48806446,     61356675,     76695844,     95443717,    119304647,
/*   16 */    148102320,    186737708,    238609294,    286331153,    357913941,
};

MALLOC_DEFINE(M_SCHED, "scheduler", "scheduler data");

static void update_curr(struct sfq *);
static void update_sfq_load_avg(struct sfq *, int);
static void update_sfq_shares(struct sfq *);


/*
 * We consider the maximum latency as the sum of the threads waiting
 * to run.
 */
static uint64_t
sched_period(unsigned long nr_running)
{
	uint64_t period = sched_latency;
	unsigned long nr_latency = sched_nr_latency;
 
	if (nr_running > nr_latency) {
		period = sched_min_granularity;
		period *= nr_running;
	}
	return (period);
}

static int
sysctl_sched_latency(SYSCTL_HANDLER_ARGS)
{
	int error;
	int val;
 
	val = sched_latency;
	error = sysctl_handle_long(oidp, &val, 0, req);
	if (error || !req->newptr)
		return (error);
 
	if (val < sched_min_granularity)
		return (EINVAL);

	sched_latency = val;
	sched_nr_latency = (sched_latency + 
		sched_min_granularity - 1) / sched_min_granularity;
        return (0);
}

static int
sysctl_sched_min_granularity(SYSCTL_HANDLER_ARGS)
{
	int error;
	int val;
 
	val = sched_min_granularity;
	error = sysctl_handle_long(oidp, &val, 0, req);
	if (error || !req->newptr)
		return (error);
 
	if (val > sched_latency)
		return (EINVAL);

	sched_min_granularity = val;
	sched_nr_latency = (sched_latency +
		sched_min_granularity - 1) / sched_min_granularity;
        return (0);
}

#ifdef SFQ_GROUP_SCHED

/* Walk up scheduling entities hierarchy */
#define WALKUP_SCHED_ENTITY(se) \
	for (; se; se = se->parent)

static inline struct rq *
sfq_to_rq(struct sfq *sfq)
{
	return (sfq->rq);
}

static inline struct sfq *
se_to_sfq(struct sched_entity *se)
{
	return (se->sfq);
}

static inline struct sfq *
group_sfq(struct sched_entity *grp)
{
	return (grp->my_q);
}

static inline int
is_same_group(struct sched_entity *a, struct sched_entity *b)
{
	if (a->sfq == b->sfq)
		return (1);
	return (0);
}

static inline struct sched_entity *
parent_entity(struct sched_entity *se)
{
	return (se->parent);
}

static inline int
entity_depth(struct sched_entity *se)
{
	int depth = 0;
 
	WALKUP_SCHED_ENTITY(se)
		depth++;
	return (depth);
}

static void
find_common_ancestor(struct sched_entity **a, struct sched_entity **b)
{
	int depth_a, depth_b;

	depth_a = entity_depth(*a);
	depth_b = entity_depth(*b);

	while (depth_a > depth_b) {
		depth_a--;
		*a = parent_entity(*a);
        }

	while (depth_b > depth_a) {
		depth_b--;
		*b = parent_entity(*b);
	}

	while (!is_same_group(*a, *b)) {
		*a = parent_entity(*a);
		*b = parent_entity(*b);
	}
}

static void
add_leaf_sfq(struct sfq *sfq)
{
	struct sfq *parent_sfq;
	struct rq *rq;

	if (sfq->on_leaf_list)
		return;
	rq = sfq_to_rq(sfq);
	/*
	 * Ensure that we always add child queue before parent,
	 * so that load_avg can be propagated to upper level correctly.
	 */
	if (sfq->group->parent && (parent_sfq =
	    sfq->group->parent->sfq[rq_cpu(rq)])->on_leaf_list) {
		TAILQ_INSERT_BEFORE(parent_sfq, sfq, leaf_link);
	} else {
		TAILQ_INSERT_TAIL(&rq->leaf_sfq_list, sfq, leaf_link);
	}
	sfq->on_leaf_list = 1;
}

static void
del_leaf_sfq(struct sfq *sfq)
{
	struct rq *rq;

	if (sfq->on_leaf_list) {
		rq = sfq_to_rq(sfq);
		TAILQ_REMOVE(&rq->leaf_sfq_list, sfq, leaf_link);
		sfq->on_leaf_list = 0;
	}
}

#else

static inline struct rq *
sfq_to_rq(struct sfq *sfq_p)
{
	return  __containerof(sfq_p, struct rq, sfq);
}

#define WALKUP_SCHED_ENTITY(se) \
	for (; se; se = NULL)

static inline struct sfq *
se_to_sfq(struct sched_entity *se)
{
	struct rq *rq;

	rq = cpu_rq(se->cpu);
	return (&rq->sfq);
}

static inline struct sfq *
group_sfq(struct sched_entity *grp)
{
	return (NULL);
}

#define FOREACH_LEAF_SFQ(rq, sfq) \
	for (sfq = &rq->sfq; sfq; sfq = NULL)

static inline int
is_same_group(struct sched_entity *a, struct sched_entity *b)
{
	return (1);
}

static inline struct sched_entity *
parent_entity(struct sched_entity *se)
{
	return (NULL);
}

static inline void
find_common_ancestor(struct sched_entity **a, struct sched_entity **b)
{
}

static inline void
add_leaf_sfq(struct sfq *sfq)
{
}

static inline void
del_leaf_sfq(struct sfq *sfq)
{
}

#endif

#if __LONG_BIT == 32
#	define WMULT_CONST	(~0UL)
#else
#	define WMULT_CONST	(1UL << 32)
#endif
        
#define	WMULT_SHIFT		32

/*
 * Round and shift right:
 */
#define RSR(x, y) (((x) + (1UL << ((y) - 1))) >> (y))

/*
 * Calculate delta * weight / load_weight
 */
static unsigned long
scale_by_weight(unsigned long delta_exec, unsigned long weight,
       struct load_weight *lw)
{
	uint64_t tmp;

	tmp = (uint64_t)delta_exec * weight;
 
	if (!lw->inv_weight) {
		unsigned long w = lw->weight;
 
		if (__LONG_BIT > 32 && w >= WMULT_CONST)
			lw->inv_weight = 1;
		else if (!w)
			lw->inv_weight = WMULT_CONST;
		else
			lw->inv_weight = WMULT_CONST / w;
	}

	/*
	 * If both A and B are larger than 2^32, then A * B will not fit in
	 * 64 bits, to avoid the overflow, we calculate it in two steps.
	 */
	if (tmp > WMULT_CONST)
		tmp = RSR(RSR(tmp, WMULT_SHIFT/2) * lw->inv_weight,
			WMULT_SHIFT/2);
	else
		tmp = RSR(tmp * lw->inv_weight, WMULT_SHIFT);

	return (unsigned long)MIN(tmp, (uint64_t)(unsigned long)LONG_MAX);
}

void
set_thread_weight(struct thread *td)
{
	struct sched_entity *se = td_to_se(td);
	int idx = td->td_proc->p_nice - PRIO_MIN;
	
	if (td->td_pri_class != PRI_IDLE) {
		se->load.weight = prio_to_weight[idx];
		se->load.inv_weight = prio_to_wmult[idx];
	} else {
		se->load.weight = IDLEPRIO_WEIGHT;
		se->load.inv_weight = IDLEPRIO_WMULT;
	}
}

static inline void
load_add(struct load_weight *lw, unsigned long inc)
{
	lw->weight += inc;
	lw->inv_weight = 0;
}

static inline void
load_sub(struct load_weight *lw, unsigned long dec)
{
	lw->weight -= dec;
	lw->inv_weight = 0;
}

static inline void
load_set(struct load_weight *lw, unsigned long w)
{
	lw->weight = w;
	lw->inv_weight = 0;
}

static inline int
vtime_cmp(struct sched_entity *a, struct sched_entity *b)
{
	if ((int64_t)(a->vruntime - b->vruntime) < 0)
		return (-1);
	return (1);
}

static inline uint64_t
entity_before(struct sched_entity *a, struct sched_entity *b)
{
	return (vtime_cmp(a, b) < 0);
}

RB_GENERATE(TIMELINE, sched_entity, tlink, vtime_cmp);

static void
sfq_init(struct sfq *sfq)
{
	RB_INIT(&sfq->timeline);
}

static void
account_entity_enqueue(struct sfq *sfq, struct sched_entity *se)
{
	load_add(&sfq->load, se->load.weight);
	if (parent_entity(se) == NULL)
		load_add(&sfq_to_rq(sfq)->load, se->load.weight);
	sfq->nr_running++;
}

static void
account_entity_dequeue(struct sfq *sfq, struct sched_entity *se)
{
	load_sub(&sfq->load, se->load.weight);
	if (parent_entity(se) == NULL)
		load_sub(&sfq_to_rq(sfq)->load, se->load.weight);
	sfq->nr_running--;
}

#ifdef SFQ_GROUP_SCHED

#ifdef SMP
static void
update_sfq_load_contribution(struct sfq *sfq, int force_update)
{
	struct sched_group *sg = sfq->group;
	long load_avg, diff_avg, abs_avg;

	load_avg = sfq->load_avg / (sfq->load_period + 1);
	diff_avg = load_avg - sfq->load_contribution;
	if (diff_avg < 0)
		abs_avg = -diff_avg;
	else
		abs_avg = diff_avg;
	if (force_update || abs_avg > sfq->load_contribution / 8) {
		atomic_add_long(&sg->load_weight, diff_avg);
		sfq->load_contribution += diff_avg;
	}
}

static void
update_sfq_load_avg(struct sfq *sfq, int force_update)
{
	struct rq *rq;
	uint64_t period = sched_loadavg_period;
	uint64_t now, delta;
	unsigned long load = sfq->load.weight;

	if (sfq->group == &root_sched_group)
		return;

	rq = sfq_to_rq(sfq);
	now = rq->clock_thread;
	delta = now - sfq->load_timestamp;
	/* truncate load history at 4 idle periods */
	if (sfq->load_timestamp > sfq->load_laststamp &&
	    now - sfq->load_laststamp > period * 4) {
		sfq->load_period = 0;
		sfq->load_avg = 0;
		delta = period - 1;
	}

	sfq->load_timestamp = now;
	sfq->load_pending_exec_time = 0;
	sfq->load_period += delta;
	
	if (load != 0) {
		sfq->load_laststamp = now;
		sfq->load_avg += load * delta;
	}

	if (force_update ||
	    sfq->load_period > period || sfq->load_period == 0) {
		update_sfq_load_contribution(sfq, force_update);
	}
	
	while (sfq->load_period > period) {
		sfq->load_period /= 2;
		sfq->load_avg /= 2;
	}

	if (sfq->curr == NULL && sfq->nr_running == 0 && sfq->load_avg == 0)
		del_leaf_sfq(sfq);
}

static inline long
calc_group_weight(struct sched_group *sg, struct sfq *sfq)
{
	long group_weight;

	/*
	 * Subtracting old contribution and adding current load make it
	 * reflect this CPU's load change more quickly.
	 */
	group_weight = sg->load_weight - sfq->load_contribution
		+ sfq->load.weight;

	return (group_weight);
}

/*
 * shares = group_shares * queue_load / group_load
 */
static long
calc_sfq_shares(struct sched_group *sg, struct sfq *sfq)
{
	long group_weight, load, shares;
 
	group_weight = calc_group_weight(sg, sfq);
	load = sfq->load.weight;
	shares = sg->shares * load;
	if (group_weight)
		shares /= group_weight;
	if (shares < MIN_SHARES)
		shares = MIN_SHARES;
	if (shares > sg->shares)
		shares = sg->shares;
	return (shares);
}

static inline void
update_entity_shares_tick(struct sfq *sfq)
{
	if (sfq->load_pending_exec_time > sched_loadavg_period) {
		update_sfq_load_avg(sfq, 0);
		update_sfq_shares(sfq);
	}
}

#else

static inline void
update_sfq_load_avg(struct sfq *sfq, int force_update)
{
}

static inline long
calc_sfq_shares(struct sched_group *sg, struct sfq *sfq)
{
	return (sg->shares);
}

static inline void
update_entity_shares_tick(struct sfq *sfq)
{
}

#endif /* !SMP */

static void
update_entity_weight(struct sfq *sfq, struct sched_entity *se,
	unsigned long weight)
{
	if (se->joined) {
		if (sfq->curr == se)
			update_curr(sfq);
		account_entity_dequeue(sfq, se);
		load_set(&se->load, weight);
		account_entity_enqueue(sfq, se);
	} else
		load_set(&se->load, weight);
}

static void
update_sfq_shares(struct sfq *sfq)
{
	struct sched_group *sg;
	struct sched_entity *se;
	long shares;

	sg = sfq->group;
	se = sg->se[rq_cpu(sfq_to_rq(sfq))];
        if (!se)
		return;
#ifndef SMP
	if (se->load.weight == sg->shares)
		return;
#endif
	shares = calc_sfq_shares(sg, sfq);
	update_entity_weight(se_to_sfq(se), se, shares);
}

#else

static inline void
update_sfq_load_avg(struct sfq *sfq, int force_update)
{
}

static inline void
update_sfq_shares(struct sfq *sfq)
{
}

static inline void
update_entity_shares_tick(struct sfq *sfq)
{
}

#endif /* !SFQ_GROUP_SCHED */

static inline void
__runq_enqueue(struct rq *rq, struct thread *td, struct sched_entity *se,
	int flags)
{

	KASSERT((se->flags & SEF_ONRUNQ) == 0, ("already on runq"));
	runq_add(&rq->realtime, td,
               (flags & SWT_PREEMPT) ? SRQ_PREEMPTED : 0);
	se->flags |= SEF_ONRUNQ;
}

static inline void
__runq_dequeue(struct rq *rq, struct thread *td, struct sched_entity *se)
{

	if (se->flags & SEF_ONRUNQ) {
		runq_remove(&rq->realtime, td);
		se->flags &= ~SEF_ONRUNQ;
	}
}

/*
 * Insert an entity into the rb-tree.
 */
static void
__enqueue_entity(struct sfq *sfq, struct sched_entity *se)
{
	struct sched_entity *tmp;
	struct sched_entity *parent = NULL;
	int comp = 0;
	int leftmost = 1;

	KASSERT((se->flags & SEF_ONSFQ) == 0, ("Already on SFQ"));
	tmp = RB_ROOT(&sfq->timeline);
	while (tmp) {
		parent = tmp;
		comp = vtime_cmp(se, tmp);
		if (comp < 0)
			tmp = RB_LEFT(tmp, tlink);
		else {
			tmp = RB_RIGHT(tmp, tlink);
			leftmost = 0;
		}
	}
	RB_SET(se, parent, tlink);
	if (parent != NULL) {
		if (comp < 0)
			RB_LEFT(parent, tlink) = se;
		else
			RB_RIGHT(parent, tlink) = se;
		RB_AUGMENT(parent);
	} else
		RB_ROOT(&sfq->timeline) = se;
	TIMELINE_RB_INSERT_COLOR(&sfq->timeline, se);

	/*
	 * Cache the leftmost entry (it is frequently used)
	 */
	if (leftmost)
		sfq->leftmost = se;
	se->flags |= SEF_ONSFQ;
}

/*
 * Remove an entity from the rb-tree.
 */
static void
__dequeue_entity(struct sfq *sfq, struct sched_entity *se)
{
	KASSERT(se->flags & SEF_ONSFQ, ("Not on SFQ"));

	if (sfq->leftmost == se)
		sfq->leftmost = TIMELINE_RB_NEXT(se);
	TIMELINE_RB_REMOVE(&sfq->timeline, se);
	se->flags &= ~SEF_ONSFQ;
}

static inline struct sched_entity *
__pick_first_entity(struct sfq *sfq)
{
	return (sfq->leftmost);
}

static inline struct sched_entity *
__pick_next_entity(struct sfq *sfq, struct sched_entity *se)
{
	return (TIMELINE_RB_NEXT(se));
}

static inline uint64_t
max_vruntime(uint64_t v, uint64_t vruntime)
{
	if ((int64_t)(vruntime - v) > 0)
		return (vruntime);
	return (v);
}

static inline uint64_t
min_vruntime(uint64_t v, uint64_t vruntime)
{
	if ((int64_t)(vruntime - v) < 0)
		return (vruntime);
	return (v);
}

/*
 * Update the scheduler virtual time, which is minimum virtual time
 * of all runnable threads includes the running thread.
 */
static void
update_sfq_vtime(struct sfq *sfq)
{
	struct sched_entity *leftmost = sfq->leftmost;
	struct sched_entity *curr = sfq->curr;
	uint64_t vruntime;

	if (curr != NULL && !curr->joined)
		curr = NULL;

	if (curr != NULL && leftmost != NULL) {
		vruntime = min_vruntime(curr->vruntime, leftmost->vruntime);
	} else if (curr != NULL && leftmost == NULL) {
		vruntime = curr->vruntime;
	} else if (curr == NULL && leftmost != NULL) {
		vruntime = leftmost->vruntime;
	} else {
		return;
	}

	sfq->vtime = max_vruntime(sfq->vtime, vruntime);
#ifndef __LP64__
#ifdef SMP
	wmb();
#endif
	sfq->vtime_copy = sfq->vtime;
#endif
}

/*
 * The parameter delta is scaled by weight, if the weight of se is
 * not default.
 */
static inline unsigned long
scale_delta(unsigned long delta_exec, struct sched_entity *se)
{
	if (se->load.weight == NICE_0_WEIGHT)
		return (delta_exec);
	return (scale_by_weight(delta_exec, NICE_0_WEIGHT, &se->load));
}

/* Return wall-time slice in proportion of its weight */
static uint64_t
sched_slice(struct sfq *sfq, struct sched_entity *se)
{
	uint64_t slice = sched_period(sfq->nr_running);

	WALKUP_SCHED_ENTITY(se) {
		struct load_weight *load;
		struct load_weight lw;

		sfq = se_to_sfq(se);
		load = &sfq->load;
		if (!se->joined) {
			lw = sfq->load;
			load_add(&lw, se->load.weight);
			load = &lw;
		}
		slice = scale_by_weight(slice, se->load.weight, load);
	}
	return (slice);
}

static inline uint64_t
sched_vslice(struct sfq *sfq, struct sched_entity *se)
{
	return scale_delta(sched_slice(sfq, se), se);
}

/*
 * Adjust an entity's start time before being added to rbtree.
 */
static void
place_entity(struct sfq *sfq, struct sched_entity *se, int initial)
{
	uint64_t min_vruntime = sfq->vtime;

	if (initial) {
		if (start_debit)
			min_vruntime += sched_vslice(sfq, se);
	} else {
		/*
		 * Allow lag behind the system time when wakeup,
		 * this makes interactive application happy!
		 * However this is not part of the SFQ thesis.
		 */
		unsigned long thresh = sched_min_granularity;
		if (gentle_sleeper)
			thresh >>= 1;
		min_vruntime -= thresh;
	}

	/*
	 * This adjustment prevents a thread from claiming an excessive
	 * share of the CPU after sleeping for a long time.
	 */
	se->vruntime = max_vruntime(se->vruntime, min_vruntime);
}

/*
 * Update the current entity's runtime statistics.
 */
static void
update_curr(struct sfq *sfq)
{
	struct rq *rq;
	struct sched_entity *curr = sfq->curr;
	uint64_t now;
	unsigned long delta_exec, delta_exec_weighted;
	struct thread *td;

	if (!curr)
		return;

	rq = sfq_to_rq(sfq);
	if (entity_is_thread(curr)) {
		td = se_to_td(curr);
		if (td->td_pri_class == PRI_TIMESHARE &&
		    td->td_user_pri != NORMAL_PRI)
			td->td_user_pri = NORMAL_PRI;
	}

	now = rq->clock_thread;
	delta_exec = (unsigned long)(now - curr->timing_start);
	if (!delta_exec)
		return;
#if defined(SMP) && defined(SFQ_GROUP_SCHED)
	sfq->load_pending_exec_time += delta_exec;
#endif
	curr->sum_exec_time += delta_exec;
	delta_exec_weighted = scale_delta(delta_exec, curr);
	curr->vruntime += delta_exec_weighted;
	curr->timing_start = now;
	update_sfq_vtime(sfq);
}

static void
set_skip_candidate(struct sched_entity *se)
{
	struct sfq *sfq;

	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		sfq->skip = se;
		if (sfq->nr_running > 1)
			break;
	}
}

static void
clear_skip_candidate(struct sched_entity *se)
{
	struct sfq *sfq;

	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		if (sfq->skip == se)
			sfq->skip = NULL;
		else
			break;
	}
}

static void
set_next_candidate(struct sched_entity *se)
{
	struct sfq *sfq;

	if (entity_is_thread(se) && se_to_td(se)->td_pri_class == PRI_IDLE)
		return;

	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		sfq->next = se;
	}
}

static void
clear_next_candidate(struct sched_entity *se)
{
	struct sfq *sfq;

	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		if (sfq->next == se)
			sfq->next = NULL;
		else
			break;
	}
}

static void
set_last_candidate(struct sched_entity *se)
{
	struct sfq *sfq;

	if (entity_is_thread(se) && se_to_td(se)->td_pri_class == PRI_IDLE)
		return;

	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		sfq->last = se;
	}
}


static void
clear_last_candidate(struct sched_entity *se)
{
	struct sfq *sfq;

	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		if (sfq->last == se)
			sfq->last = NULL;
		else
			break;
	}
}

static void
clear_candidates(struct sfq *sfq, struct sched_entity *se)
{
	if (sfq->skip == se)
		clear_skip_candidate(se);

	if (sfq->next == se)
		clear_next_candidate(se);

	if (sfq->last == se)
		clear_last_candidate(se);
}

static void
put_prev_entity(struct sfq *sfq, struct sched_entity *se, int flags)
{
	struct rq *rq;
	struct thread *td;

	if (se->joined) {
		if (entity_is_thread(se) &&
		    (flags & SW_TYPE_MASK) == SWT_RELINQUISH)
			set_skip_candidate(se);
		update_curr(sfq);
		if (entity_is_thread(se)) {
			rq = sfq_to_rq(sfq);
			td = se_to_td(se);
			TD_SET_RUNQ(td);
			if (td->td_priority < NORMAL_PRI)
				__runq_enqueue(rq, td, se, flags);
		}

		/*
		 * Timesharing thread should always be inserted into SFQ to
		 * track its statistics.
		 */
		__enqueue_entity(sfq, se);
	}
	sfq->curr = NULL;
}

static void
pss_put_prev_thread(struct rq *rq, struct thread *td, int flags)
{
	struct sfq *sfq;
	struct sched_entity *se;

	RQ_LOCK_ASSERT(rq, MA_OWNED);
	se = td_to_se(td);
	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		put_prev_entity(sfq, se, flags);
	}
}

static int wakeup_preempt_entity(struct sched_entity *, struct sched_entity *);

static struct sched_entity *
pick_next_entity(struct sfq *sfq)
{
	struct sched_entity *first = __pick_first_entity(sfq);
	struct sched_entity *se = first;

	if (sfq->skip == se) {
		struct sched_entity *second = __pick_next_entity(sfq, se);
		if (second && wakeup_preempt_entity(second, first) < 1)
			se = second;
	}

	if (sfq->last && wakeup_preempt_entity(sfq->last, first) < 1)
		se = sfq->last;

	if (sfq->next && wakeup_preempt_entity(sfq->next, first) < 1)
		se = sfq->next;

	clear_candidates(sfq, se);

	return (se);
}

static struct thread *
pss_pick_next_thread(struct rq *rq)
{
	struct sfq *sfq = &rq->sfq;
	struct sched_entity *se;
	struct thread *td;

	if (sfq->nr_running == 0)
		return (NULL);
	do {
		se = pick_next_entity(sfq);
		sfq = group_sfq(se);
	} while(sfq != NULL);

	td = se_to_td(se);
	return (td);
}

static void
activate_entity(struct sfq *sfq, struct sched_entity *se)
{
	struct rq *rq;
	struct thread *td;

	MPASS(se->joined != 0);
	rq = sfq_to_rq(sfq);
	if (entity_is_thread(se)) {
		td = se_to_td(se);
		__runq_dequeue(rq, td, se);
	}
	__dequeue_entity(sfq, se);
	KASSERT(sfq->curr == NULL, ("curr already exists"));
	sfq->curr = se; /* The thread becomes the current. */
	se->timing_start = rq->clock_thread;
	se->prev_sum_exec_time = se->sum_exec_time;
}

static void
pss_activate_thread(struct rq *rq, struct thread *td)
{
	struct sfq *sfq;
	struct sched_entity *se;

	se = td_to_se(td);
	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		activate_entity(sfq, se);
	}
	TD_SET_RUNNING(td);
}

static void
enqueue_entity(struct sfq *sfq, struct sched_entity *se, int flags)
{
	struct rq *rq;

	MPASS(se->joined == 0);

	rq = sfq_to_rq(sfq);
	if (!(flags & ENQUEUE_WAKEUP))
		se->vruntime += sfq->vtime;
	update_curr(sfq);
	update_sfq_load_avg(sfq, 0);
	if (flags & ENQUEUE_WAKEUP)
		place_entity(sfq, se, 0);

	if (entity_is_thread(se)) {
		struct thread *td = se_to_td(se);
		TD_SET_RUNQ(td);
		if (td->td_priority < NORMAL_PRI)
			__runq_enqueue(rq, td, se, flags);
	}

	__enqueue_entity(sfq, se);
	account_entity_enqueue(sfq, se);
	update_sfq_shares(sfq);
	if (sfq->nr_running == 1)
		add_leaf_sfq(sfq);
	se->joined = 1;
}

static void
pss_enqueue_thread(struct rq *rq, struct thread *td, int flags)
{
	struct sfq *sfq;
	struct sched_entity *se;

	se = td_to_se(td);
	WALKUP_SCHED_ENTITY(se) {
		if (se->joined)
			break;
		sfq = se_to_sfq(se);
		enqueue_entity(sfq, se, flags);
		flags = ENQUEUE_WAKEUP;
	}
	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		update_sfq_load_avg(sfq, 0);
		update_sfq_shares(sfq);
	}

	rq_run_add(rq, td);
}

static void
dequeue_entity(struct sfq *sfq, struct sched_entity *se, int flags)
{
	struct rq *rq = sfq_to_rq(sfq);

	if (se->joined == 0) {
		KASSERT((se->flags & (SEF_ONRUNQ | SEF_ONSFQ)) == 0,
			("Inconsistent queue state"));
		return;
	}
	update_curr(sfq);
	update_sfq_load_avg(sfq, 0);
	if (entity_is_thread(se))
		__runq_dequeue(rq, se_to_td(se), se);
	clear_candidates(sfq, se);
	if (se->flags & SEF_ONSFQ)
		__dequeue_entity(sfq, se);
	account_entity_dequeue(sfq, se);
	se->joined = 0;
	if (!(flags & DEQUEUE_SLEEP))
		se->vruntime -= sfq->vtime;
	update_sfq_vtime(sfq);
	update_sfq_shares(sfq);
}

static void
pss_dequeue_thread(struct rq *rq, struct thread *td, int flags)
{
	struct sfq *sfq;
	struct sched_entity *se;
	int is_sleep = flags & DEQUEUE_SLEEP;

	se = td_to_se(td);
	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		dequeue_entity(sfq, se, flags);
		if (sfq->load.weight) {
			/*
			 * Prefer to pick entity in the same group,
			 * because we are going to sleep not because we
			 * have used up time slice. This reduces number of
			 * address space switch.
			 */
			if (is_sleep && parent_entity(se)) {
				set_next_candidate(parent_entity(se));
			}
			/*
			 * dequeue has already called update_sfq_shares,
			 * don't repeat it again, see below.
			 */
			se = parent_entity(se);
			break;
		}
		flags |= DEQUEUE_SLEEP;
	}
	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		update_sfq_load_avg(sfq, 0);
		update_sfq_shares(sfq);
	}

	rq_run_rem(rq, td);
}

static void
pss_change_priority(struct rq *rq, struct thread *td, u_char prio)
{
	struct sched_entity *se;

	se = td_to_se(td);
	/*
	 * If the priority has been elevated due to priority
	 * propagation, we may have to move ourselves to a new
	 * queue. This could be optimized to not re-add in some
	 * cases.
	 */
	if (TD_ON_RUNQ(td) && prio < td->td_priority) {
		__runq_dequeue(rq, td, se);
		td->td_priority = prio;
		if (prio < NORMAL_PRI)
			__runq_enqueue(rq, td, se, 0);
		check_priority_preempt(rq, td);
		return;
	}
	td->td_priority = prio;
}

static unsigned long
wakeup_gran(struct sched_entity *cur, struct sched_entity *new_se)
{
	/*
	 * Use new_se to compute result, this penalizes light thread,
	 * a light thread will return larger value and cause
	 * check_preempt_curr() to be more unwilling to preempt
	 * current.
	 */
	return scale_delta(wakeup_granularity, new_se);
}

static int
wakeup_preempt_entity(struct sched_entity *curr, struct sched_entity *se)
{
	int64_t diff, gran;
	
	diff = (int64_t)(curr->vruntime - se->vruntime);
	if (diff > 0) {
		gran = wakeup_gran(curr, se);
		if (diff > gran)
			return (1);
		return (0);
	}
	return (-1);
}

static void
pss_check_preempt_curr(struct rq *rq, struct thread *td)
{
	struct thread *ctd = rq_curthread(rq);
	struct sched_entity *curr, *se;
	struct sfq *sfq;
	int scale;

	if (thread_need_resched(ctd))
		return;
	curr = td_to_se(ctd);
	se = td_to_se(td);
	sfq = se_to_sfq(curr);
	scale = (sfq->nr_running >= sched_nr_latency);
	if (ctd->td_pri_class == PRI_IDLE && td->td_pri_class != PRI_IDLE)
		goto preempt;
	find_common_ancestor(&curr, &se);
	update_curr(se_to_sfq(curr));
	if (wakeup_preempt_entity(curr, se) > 0) {
		/*
		 * Wakeup/sleep optimization, it is possible they share
		 * data.
		 */
		set_next_candidate(se);
		goto preempt;
	}
	return;
preempt:
	resched_thread(ctd);

	/*
	 * Set last candidate to the preempted thread if the load is too high,
	 * This prefers cache affinity than fairness because latency is no longer
	 * guaranteed (because nr_running > sched_nr_latency).
	 */
	if (sched_last && scale && entity_is_thread(se))
		set_last_candidate(se);
}

static void
check_preempt_tick(struct sfq *sfq, struct sched_entity *se)
{
	struct thread *ctd = curthread;
	struct sched_entity *leftmost;
	unsigned long ideal_slice, delta_exec;
	int64_t delta;

	ideal_slice = sched_slice(sfq, se);
	delta_exec = se->sum_exec_time - se->prev_sum_exec_time;
	if (delta_exec > ideal_slice) {
		clear_candidates(sfq, se);
		resched_thread(ctd);
		return;
	}
	if (delta_exec < sched_min_granularity)
		return;
	if ((leftmost = __pick_first_entity(sfq)) == NULL)
		return;
	delta = (int64_t)(se->vruntime - leftmost->vruntime);
	if (delta <= 0)
		return;
	if (delta > (int64_t)ideal_slice)
		resched_thread(ctd);
}

static void
entity_tick(struct sfq* sfq, struct sched_entity *se)
{
	update_curr(sfq);
	update_entity_shares_tick(sfq);
	if (sfq->nr_running > 1)
		check_preempt_tick(sfq, se);
}

static void
pss_tick_thread(struct rq *rq, struct thread *td)
{
	struct sched_entity *se;
	struct sfq *sfq;

	se = td_to_se(td);
	RQ_LOCK_ASSERT(rq, MA_OWNED);
	WALKUP_SCHED_ENTITY(se) {
		sfq = se_to_sfq(se);
		entity_tick(sfq, se);
	}
}

static void
pss_fork_thread(struct rq *rq, struct thread *curr, 
	struct thread *childtd)
{
	struct sched_entity *se = td_to_se(curr);
	struct sched_entity *se2 = td_to_se(childtd);
	struct sfq *sfq = se_to_sfq(se);

	update_rq_clock(rq);
	update_curr(sfq);

	se2->vruntime = se->vruntime;
	place_entity(sfq, se2, 1);
	if (child_runs_first && entity_before(se, se2)) {
		uint64_t tmp;

		tmp = se->vruntime;
		se->vruntime = se2->vruntime;
		se2->vruntime = tmp;
		resched_thread(curr);
	}
	se2->vruntime -= sfq->vtime;
}

static void
pss_switched_from(struct rq *rq, struct thread *td)
{
	struct sched_entity *se = td_to_se(td);
	struct sfq *sfq = se_to_sfq(se);

	if (!se->initial && !TD_ON_RUNQ(td) && !TD_IS_RUNNING(td)) {
		/*
		 * Only sleeping thread's vruntime was not normalized, when
		 * it leaves the class, we want to normalize it, this
		 * prevents it from claiming an excessive share of the CPU
		 * when it comes back.
		 */
		place_entity(sfq, se, 0);
		se->vruntime -= sfq->vtime;
	}
}

static void
pss_switched_to(struct rq *rq, struct thread *td)
{
	struct sched_entity *se = td_to_se(td);
	struct sfq *sfq = se_to_sfq(se);
	
	if (!se->initial && !TD_ON_RUNQ(td) && !TD_IS_RUNNING(td)) {
		se->vruntime += sfq->vtime;
		return;
	}

	if (rq_curthread(rq) == td)
		resched_thread(td);
	else 
		check_preempt_curr(rq, td);
}

#ifdef SFQ_GROUP_SCHED

void pss_init_group(struct sched_group *sg, struct sfq *sfq,
	struct sched_entity *se, int cpu,
	struct sched_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	sfq->group = sg;
	sfq->rq = rq;

	sg->sfq[cpu] = sfq;
	sg->se[cpu] = se;

	/* se could be NULL for root_sched_group */
	if (se == NULL)
		return;

	if (parent == NULL)
		se->sfq = &rq->sfq;
	else
		se->sfq = parent->my_q;

	se->my_q = sfq;
	se->parent = parent;
	load_set(&se->load, 0);
}

int
pss_alloc_sched_group(struct sched_group *sg, struct sched_group *parent, unsigned shares)
{
	struct sfq *sfq;
	struct sched_entity *se;
        int i;
 
	MPASS(mp_ncpus == mp_maxid + 1);

	sg->sfq = malloc(sizeof(void *) * mp_ncpus, M_SCHED,
		M_WAITOK | M_ZERO);
	if (sg->sfq == NULL)
		return (0);
	sg->se = malloc(sizeof(void *) * mp_ncpus, M_SCHED,
		M_WAITOK| M_ZERO);
	if (sg->se == NULL)
		return (0);

	if (shares == 0)
		sg->shares = NICE_0_WEIGHT;
	else {
		if (shares < MIN_SHARES)
			shares = MIN_SHARES;
		else if (shares > MAX_SHARES)
			shares = MAX_SHARES;
		sg->shares = shares;
	}

	CPU_FOREACH(i) {
		sfq = uma_zalloc(sfq_zone, M_WAITOK | M_ZERO);
		if (sfq == NULL)
			return (0);

		se = uma_zalloc(entity_zone, M_WAITOK | M_ZERO);
		if (se == NULL) {
			uma_zfree(sfq_zone, sfq);
			return (0);
		}
		sfq_init(sfq);
		pss_init_group(sg, sfq, se, i, parent->se[i]);
	}

	return (1);
}

void
pss_free_sched_group(struct sched_group *sg)
{
	int i;
 
	CPU_FOREACH(i) {
		if (sg->sfq != NULL)
			uma_zfree(sfq_zone, sg->sfq[i]);
		if (sg->se != NULL)
			uma_zfree(entity_zone, sg->se[i]);
        }
	free(sg->sfq, M_SCHED);
	free(sg->se, M_SCHED);
}

static void
pss_sched_group_move(struct thread *td, int on_rq)
{
	struct sched_entity *se = td_to_se(td);

	if (!se->initial && !on_rq)
		se->vruntime -= se_to_sfq(se)->vtime;
	set_thread_rq(td, thread_cpu(td));
	if (!se->initial && !on_rq)
		se->vruntime += se_to_sfq(se)->vtime;
}

int
pss_sched_group_set_shares(struct sched_group *sg, unsigned long shares)
{
	struct rq *rq;
	struct sched_entity *se;
	int i;
	
	if (!sg->se[0]) /* root group ? */
		return (EINVAL);
	
	if (shares == 0)
		sg->shares = NICE_0_WEIGHT;
	else {
		if (shares < MIN_SHARES)
			shares = MIN_SHARES;
		else if (shares > MAX_SHARES)
			shares = MAX_SHARES;
	}
	mtx_lock(&shares_lock);
	if (shares == sg->shares)
		goto done;
	sg->shares = shares;
	CPU_FOREACH(i) {
		rq = cpu_rq(i);
		se = sg->se[i];
		RQ_LOCK(rq);
		WALKUP_SCHED_ENTITY(se) {
			update_sfq_shares(se_to_sfq(se));
		}
		RQ_UNLOCK(rq);
	}
done:
	mtx_unlock(&shares_lock);
	return (0);
}

void
pss_unregister_sched_group(struct sched_group *sg, int cpu)
{
	struct rq *rq;

	rq = cpu_rq(cpu);
	RQ_LOCK(rq);
	del_leaf_sfq(sg->sfq[cpu]);
	RQ_UNLOCK(rq);
}

#endif

static void
pss_init_rq(struct rq *rq)
{
	sfq_init(&rq->sfq);
}

#define ilog2(n)	(fls(n) -1)

void
pss_sched_init(void)
{
	int factor;

	TUNABLE_INT_FETCH("kern.sched.sched_scaling", &sched_scaling);
	switch (sched_scaling) {
	case SCHED_SCALING_NONE:
		factor = 1;
                break;
	case SCHED_SCALING_LINEAR:
		factor = mp_ncpus;
                break;
	case SCHED_SCALING_LOG:
	default:
		factor = 1 + ilog2(mp_ncpus);
		break;
	}
	sched_latency *= factor;
	sched_min_granularity *= factor;

#ifdef SFQ_GROUP_SCHED
	mtx_init(&shares_lock, "group shares lock", NULL, MTX_DEF);
#endif
}

struct sched_class pss_class = {
	.next = &idle_class,
	.name = "pss",
	.init_rq = pss_init_rq,
	.put_prev_thread = pss_put_prev_thread,
	.pick_next_thread = pss_pick_next_thread,
	.activate_thread = pss_activate_thread,
	.sched_tick = pss_tick_thread,
	.change_priority = pss_change_priority,
	.check_preempt_curr = pss_check_preempt_curr,
	.enqueue_thread = pss_enqueue_thread,
	.dequeue_thread = pss_dequeue_thread,
	.fork_thread = pss_fork_thread,
	.switched_from = pss_switched_from,
	.switched_to = pss_switched_to,
#ifdef SFQ_GROUP_SCHED
	.sched_group_move = pss_sched_group_move
#endif
};

SYSCTL_LONG(_kern_sched, OID_AUTO, wakeup_granularity, CTLFLAG_RW,
    &wakeup_granularity, 0, "Wakeup preemption granularity");
SYSCTL_PROC(_kern_sched, OID_AUTO, sched_latency, CTLTYPE_ULONG | CTLFLAG_RW,
    0, sizeof(long), sysctl_sched_latency, "LU", "scheduler latency period");
SYSCTL_PROC(_kern_sched, OID_AUTO, sched_min_granularity,
    CTLTYPE_ULONG | CTLFLAG_RW,
    0, sizeof(long), sysctl_sched_min_granularity, "LU", "scheduler min granularity");
SYSCTL_INT(_kern_sched, OID_AUTO, gentle_sleeper, CTLTYPE_INT | CTLFLAG_RW,
    &gentle_sleeper, 0, "reduce sleeper lag");
SYSCTL_INT(_kern_sched, OID_AUTO, start_debit, CTLTYPE_INT | CTLFLAG_RW,
    &start_debit, 0, "new thread vruntime debit");
SYSCTL_INT(_kern_sched, OID_AUTO, child_runs_first, CTLTYPE_INT | CTLFLAG_RW,
    &child_runs_first, 0, "Child process runs first");
SYSCTL_INT(_kern_sched, OID_AUTO, sched_scaling, CTLTYPE_INT | CTLFLAG_RD,
    &sched_scaling, 0, "SMP scaling factor");
SYSCTL_INT(_kern_sched, OID_AUTO, sched_loadavg_period,
    CTLTYPE_INT | CTLFLAG_RD, &sched_loadavg_period, 0, "Load average period");
SYSCTL_INT(_kern_sched, OID_AUTO, sched_last, CTLTYPE_INT | CTLFLAG_RW,
    &sched_last, 0, "Select last preempted thread");
