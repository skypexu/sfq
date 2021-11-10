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
 * This file implements the SFQ scheduler. SFQ is based on Start-time Fair
 * Queueing algorithm.
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

#include <machine/cpu.h>
#include <machine/smp.h>

#include <sys/resource.h>
#include <sys/resourcevar.h>
#include <sys/sdt.h>
#include <sys/sysproto.h>
#include <sys/sx.h>
#include <sys/turnstile.h>
#include <sys/umtx.h>
#include <sys/vmmeter.h>
#include <sys/cpuset.h>
#include <sys/sbuf.h>

#ifdef HWPMC_HOOKS
#include <sys/pmckern.h>
#endif

#ifdef KDTRACE_HOOKS
#include <sys/dtrace_bsd.h>
int				dtrace_vtime_active;
dtrace_vtime_switch_func_t	dtrace_vtime_switch_func;
#endif

#define THREAD_CAN_MIGRATE(td)  ((td)->td_pinned == 0)

/*
 * Cpu percentage computation macros and defines.
 *
 * SCHED_TICK_SECS:	Number of seconds to average the cpu usage across.
 * SCHED_TICK_TARG:	Number of hz ticks to average the cpu usage across.
 * SCHED_TICK_MAX:	Maximum number of ticks before scaling back.
 * SCHED_TICK_SHIFT:	Shift factor to avoid rounding away results.
 * SCHED_TICK_HZ:	Compute the number of hz ticks for a given ticks count.
 * SCHED_TICK_TOTAL:	Gives the amount of time we've been recording ticks.
 */
#define	SCHED_TICK_SECS		10
#define	SCHED_TICK_TARG		(hz * SCHED_TICK_SECS)
#define	SCHED_TICK_MAX		(SCHED_TICK_TARG + hz)
#define	SCHED_TICK_SHIFT	10
#define	SCHED_TICK_HZ(ts)	((ts)->ticks >> SCHED_TICK_SHIFT)
#define	SCHED_TICK_TOTAL(ts)	(max((ts)->ltick - (ts)->ftick, hz))

static int preempt_thresh = PRI_MIN_KERN;

/*
 * These parameters determine the slice behavior for batch work.
 */
#define	SCHED_SLICE_DEFAULT_DIVISOR	10	/* 100 ms */
#define	SCHED_SLICE_MIN_DIVISOR		5	/* DEFAULT/MIN = ~20 ms. */

/*
 * realstathz:		stathz is sometimes 0 and run off of hz.
 * sched_slice:		Runtime of each thread before rescheduling.
 * preempt_thresh:	Priority threshold for preemption and remote IPIs.
 */
static int realstathz = 127;	/* reset during boot. */
static int sched_slice = 100;	/* reset during boot. */
static int sched_slice_min = 20;	/* reset during boot. */
static int tick_ns = 1000000;
static int prio_boost = PZERO;

/*
 * part of the period that we allow rt tasks to run in us.
 * default: 0.95s
 */
int sysctl_sched_rt_runtime = 950000;

struct cpupri root_cpupri;

#ifdef SFQ_GROUP_SCHED
uma_zone_t entity_zone;
uma_zone_t sfq_zone;
uma_zone_t group_zone;
struct sched_group	root_sched_group;
static struct mtx sg_id_lock;
static struct mtx sched_group_lock;
static struct unrhdr *sg_id_unrhdr;
TAILQ_HEAD(,sched_group) all_sched_groups;
struct sx sg_tree_lock;

static void	sched_group_register_thread(struct thread *);
static void	sched_group_unregister_thread(struct thread *);
#endif

struct td_sched  ts0;
#ifdef SMP
struct rq	rqs[MAXCPU];
#else
struct rq	rqs[1];
#endif

struct pcpuidlestat {
	u_int idlecalls;
	u_int oldidlecalls;
};
static DPCPU_DEFINE(struct pcpuidlestat, idlestat);

static void sched_pctcpu_update(struct td_sched *, int);

static void sched_setup(void *dummy);
SYSINIT(sched_setup, SI_SUB_RUN_QUEUE, SI_ORDER_FIRST, sched_setup, NULL);

static void sched_initticks(void *dummy);
SYSINIT(sched_initticks, SI_SUB_CLOCKS, SI_ORDER_THIRD, sched_initticks,
   NULL);

SDT_PROVIDER_DEFINE(sched);


void
start_bandwidth_timer(struct hrtimer *timer, const struct timespec *period)
{
	struct timespec now, expiry;

	if (hrtimer_active(timer))
		return;

	hrtimer_cb_get_time(timer, &now);
	hrtimer_forward(timer, &now, period);
	hrtimer_get_expiry(timer, &expiry);
	hrtimer_start(timer, &expiry, HRTIMER_MODE_ABS);
}

/*
 * cpu load decay formula:
 * load = (2^idx - 1) / 2^idx * load + 1 / 2^idx * cur_load
 * if a cpu missed update for n-1 times, decayed load is:
 * load = (2^idx - 1) / 2^idx ^ (n-1) * load + 
 *        (2^idx - 1) / 2^idx * load + 1 / 2^idx * cur_load
 * to avoid looping n-1 times to compute the load, we pre-compute
 * power of two decay factors, for example, a missing updates for 7 times,
 * because 7 = 2^0 + 2^1 + 2^2 times, we pre-compute decay factors of
 * 1, 2, 4. Note that data in the array is scaled up by 128.
 */
#define DECAY_SHIFT 		7
static const unsigned char
	decay_to_zero_ticks[CPU_LOAD_IDX_MAX] = {0, 8, 32, 64, 128};
static int power_of_2_decay_factors[CPU_LOAD_IDX_MAX][DECAY_SHIFT + 1] = {
	{0, 0, 0, 0, 0, 0, 0, 0},
	{64, 32, 8, 0, 0, 0, 0, 0},
	{96, 72, 40, 12, 1, 0, 0, 0},
	{112, 98, 75, 43, 15, 1, 0, 0},
	{120, 112, 98, 76, 45, 16, 2, 0}
};

static unsigned long
decay_cpu_load_missed(unsigned long load, unsigned long missed_updates, int idx)
{
	int j;

	if (missed_updates == 0)
		return (load);
	if (missed_updates >= decay_to_zero_ticks[idx])
		return (0);
	if (idx == 1)
		return (load >> missed_updates);
	j = 0;
	while (missed_updates) {
		if (missed_updates & 1) {
			load = (load * power_of_2_decay_factors[idx][j]) >>
				DECAY_SHIFT;
		}
		missed_updates >>= 1;
		j++;
	}
	return (load);
}

static void
__update_cpu_load(unsigned long cpu_load[], unsigned long cur_load,
	unsigned long pending_updates)
{
	unsigned long old_load, new_load;
	int i, scale;

	cpu_load[0] = cur_load;
	for (i = 1, scale = 2; i < CPU_LOAD_IDX_MAX; ++i, scale += scale) {
		old_load = cpu_load[i];
		old_load = decay_cpu_load_missed(old_load, pending_updates - 1,
			i);
		new_load = cur_load;
		if (new_load > old_load) {
			/*
			 * Round up to avoid getting stuck while load is
			 * increasing.
			 */
			new_load += scale - 1;
		}
		cpu_load[i] = (old_load * (scale - 1) + new_load) >> i;
	}
}

static void
update_cpu_load(struct rq *rq, int cnt)
{
	rq->last_cpu_load_tick = ticks;
	__update_cpu_load(rq->cpu_load, rq->load.weight, cnt);
}

/*
 * Set owepreempt if necessary.  Preemption never happens directly in SFQ,
 * we always request it once we exit a critical section.
 */
void
check_priority_preempt(struct rq *rq, struct thread *td)
{
	struct thread *ctd;
	u_char cpri;
	u_char pri;

	RQ_LOCK_ASSERT(rq, MA_OWNED);
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	
	ctd = rq_curthread(rq);
	pri = td->td_priority;
	cpri = ctd->td_priority;
	if (pri >= cpri)
		return;
	resched_thread(ctd);
	if (panicstr != NULL || cold || TD_IS_INHIBITED(ctd) ||
	    TD_IS_IDLETHREAD(ctd))
		return;
	if (pri <= preempt_thresh)
		preempt_thread(ctd);
}

/*
 * Load is maintained for all threads RUNNING and ON_RUNQ.  Add the load
 * for this thread to the referenced thread queue.
 */
void
rq_run_add(struct rq *rq, struct thread *td)
{

	RQ_LOCK_ASSERT(rq, MA_OWNED);
	THREAD_LOCK_ASSERT(td, MA_OWNED);

	rq->nr_running++;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		rq->nr_sysrunning++;
}

/*
 * Remove the load from a thread that is transitioning to a sleep state or
 * exiting.
 */
void 
rq_run_rem(struct rq *rq, struct thread *td)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	RQ_LOCK_ASSERT(rq, MA_OWNED);
	KASSERT(rq->nr_running != 0,
		("rq_run_rem: Removing with 0 load on queue %d", rq_cpu(rq)));

	rq->nr_running--;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		rq->nr_sysrunning--;
}

/*
 * Bound timeshare latency by decreasing slice size as load increases.  We
 * consider the maximum latency as the sum of the threads waiting to run
 * aside from curthread and target no more than sched_slice latency but
 * no less than sched_slice_min runtime.
 */
int
rq_slice(struct rq *rq)
{
	int load;

	/*
	 * It is safe to use sys_load here because this is called from
	 * contexts where timeshare threads are running and so there
	 * cannot be higher priority load in the system.
	 */
	load = rq->nr_sysrunning - 1;
	if (load >= SCHED_SLICE_MIN_DIVISOR)
		return (sched_slice_min);
	if (load <= 1)
		return (sched_slice);
	return (sched_slice / load);
}

uint64_t
rq_slice_ns(struct rq *rq)
{
	uint64_t slice = rq_slice(rq);
	return (slice * tick_ns);
}

/*
 * This is only somewhat accurate since given many processes of the same
 * priority they will switch when their slices run out, which will be
 * at most sched_slice stathz ticks.
 */
int
sched_rr_interval(void)
{

	return (sched_slice);
}

/*
 * Adjust the priority of a thread.  Move it to the appropriate run-queue
 * if necessary.  This is the back-end for several priority related
 * functions.
 */
static void
sched_thread_priority(struct thread *td, u_char prio)
{
	struct rq *rq;
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	if (td->td_priority == prio)
		return;
	rq = thread_rq(td);
	ts = td->td_sched;
	RQ_LOCK(rq);
	ts->class->change_priority(rq, td, prio);
	RQ_UNLOCK(rq);
}

/*
 * Update a thread's priority when it is lent another thread's
 * priority.
 */
void
sched_lend_prio(struct thread *td, u_char prio)
{

	td->td_flags |= TDF_BORROWING;
	sched_thread_priority(td, prio);
}

/*
 * Restore a thread's priority when priority propagation is
 * over.  The prio argument is the minimum priority the thread
 * needs to have to satisfy other possible priority lending
 * requests.  If the thread's regular priority is less
 * important than prio, the thread will keep a priority boost
 * of prio.
 */
void
sched_unlend_prio(struct thread *td, u_char prio)
{
	u_char base_pri;

	if (td->td_base_pri >= PRI_MIN_TIMESHARE &&
	    td->td_base_pri <= PRI_MAX_TIMESHARE)
		base_pri = td->td_user_pri;
	else
		base_pri = td->td_base_pri;
	if (prio >= base_pri) {
		td->td_flags &= ~TDF_BORROWING;
		sched_thread_priority(td, base_pri);
	} else
		sched_lend_prio(td, prio);
}

/*
 * Standard entry for setting the priority to an absolute value.
 */
void
sched_prio(struct thread *td, u_char prio)
{
	u_char oldprio;

	/* First, update the base priority. */
	td->td_base_pri = prio;

	/*
	 * If the thread is borrowing another thread's priority, don't
	 * ever lower the priority.
	 */
	if (td->td_flags & TDF_BORROWING && td->td_priority < prio)
		return;

	/* Change the real priority. */
	oldprio = td->td_priority;
	sched_thread_priority(td, prio);

	/*
	 * If the thread is on a turnstile, then let the turnstile update
	 * its state.
	 */
	if (TD_ON_LOCK(td) && oldprio != prio)
		turnstile_adjust(td, oldprio);
}

/*
 * Set the base user priority, does not effect current running priority.
 */
void
sched_user_prio(struct thread *td, u_char prio)
{

	td->td_base_user_pri = prio;
	if (td->td_lend_user_pri <= prio)
		return;
	td->td_user_pri = prio;
}

void
sched_lend_user_prio(struct thread *td, u_char prio)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	td->td_lend_user_pri = prio;
	td->td_user_pri = min(prio, td->td_base_user_pri);
	if (td->td_priority > td->td_user_pri)
		sched_prio(td, td->td_user_pri);
	else if (td->td_priority != td->td_user_pri)
		resched_thread(td);
}

/*
 * Variadic version of thread_lock_unblock() that does not assume td_lock
 * is blocked.
 */
static inline void
thread_unblock_switch(struct thread *td, struct mtx *mtx)
{
	atomic_store_rel_ptr((volatile uintptr_t *)&td->td_lock,
	    (uintptr_t)mtx);
}

static void
enqueue_thread(struct rq *rq, struct thread *td, int flags)
{
	RQ_LOCK_ASSERT(rq, MA_OWNED);
	update_rq_clock(rq);
	td->td_sched->class->enqueue_thread(rq, td, flags);
}

static void
dequeue_thread(struct rq *rq, struct thread *td, int flags)
{
	RQ_LOCK_ASSERT(rq, MA_OWNED);
	update_rq_clock(rq);
	td->td_sched->class->dequeue_thread(rq, td, flags);
}

static void
put_prev_thread(struct rq *rq, struct thread *td, int flags)
{
	RQ_LOCK_ASSERT(rq, MA_OWNED);
	if (td->td_sched->se.joined)
		update_rq_clock(rq);
	td->td_sched->class->put_prev_thread(rq, td, flags);
}

static void
activate_thread(struct rq *rq, struct thread *td)
{
	RQ_LOCK_ASSERT(rq, MA_OWNED);
	td->td_sched->class->activate_thread(rq, td);
	cpupri_set(&root_cpupri, 0, td->td_priority);
}

/*
 * Switch threads.  This function has to handle threads coming in while
 * blocked for some reason, running, or idle.  It also must deal with
 * migrating a thread from one queue to another as running threads may
 * be assigned elsewhere via binding.
 */
void
sched_switch(struct thread *td, struct thread *newtd, int flags)
{
	struct rq *rq;
	struct td_sched *ts;
	struct mtx *mtx;
	int cpuid;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	KASSERT(newtd == NULL, ("sched_switch: Unsupported newtd argument"));

	cpuid = PCPU_GET(cpuid);
	rq = cpu_rq(cpuid);
	ts = td->td_sched;
	mtx = td->td_lock;
	sched_pctcpu_update(ts, 1);
	td->td_oncpu = NOCPU;
	td->td_owepreempt = 0;
	td->td_flags &= ~(TDF_NEEDRESCHED | TDF_SLICEEND);
	if (!TD_IS_IDLETHREAD(td))
		rq->switchcnt++;
	/*
	 * The lock pointer in an idle thread should never change.  Reset it
	 * to CAN_RUN as well.
	 */
	if (TD_IS_RUNNING(td)) {
		MPASS(td->td_lock == RQ_LOCKPTR(rq));
		put_prev_thread(rq, td, flags);
	} else {
		/* This thread must be going to sleep. */
		RQ_LOCK(rq);
		dequeue_thread(rq, td, DEQUEUE_SLEEP);
		put_prev_thread(rq, td, flags);
		mtx = thread_lock_block(td);
	}

	/*
	 * We enter here with the thread blocked and assigned to the
	 * appropriate cpu run-queue or sleep-queue and with the current
	 * thread-queue locked.
	 */
	RQ_LOCK_ASSERT(rq, MA_OWNED | MA_NOTRECURSED);
	newtd = choosethread();
	/*
	 * Call the MD code to switch contexts if necessary.
	 */
	if (td != newtd) {
#ifdef	HWPMC_HOOKS
		if (PMC_PROC_IS_USING_PMCS(td->td_proc))
			PMC_SWITCH_CONTEXT(td, PMC_FN_CSW_OUT);
#endif
		RQ_LOCKPTR(rq)->mtx_lock = (uintptr_t)newtd;
		sched_pctcpu_update(newtd->td_sched, 0);

#ifdef KDTRACE_HOOKS
		/*
		 * If DTrace has set the active vtime enum to anything
		 * other than INACTIVE (0), then it should have set the
		 * function to call.
		 */
		if (dtrace_vtime_active)
			(*dtrace_vtime_switch_func)(newtd);
#endif

		cpu_switch(td, newtd, mtx);

#ifdef	HWPMC_HOOKS
		if (PMC_PROC_IS_USING_PMCS(td->td_proc))
			PMC_SWITCH_CONTEXT(td, PMC_FN_CSW_IN);
#endif
	} else {
		thread_unblock_switch(td, mtx);
	}
	/*
	 * Assert that all went well and return.
	 */
	RQ_LOCK_ASSERT(rq, MA_OWNED|MA_NOTRECURSED);
	MPASS(td->td_lock == RQ_LOCKPTR(rq));
	td->td_oncpu = cpuid;
}

/*
 * Adjust thread priorities as a result of a nice request.
 */
void
sched_nice(struct proc *p, int nice)
{
	struct rq *rq;
	struct thread *td;
	int on_rq, running;
	int delta;

	PROC_LOCK_ASSERT(p, MA_OWNED);

	delta = p->p_nice - nice;
	p->p_nice = nice;
	FOREACH_THREAD_IN_PROC(p, td) {
		thread_lock(td);
		if (td->td_sched->class != &pss_class) {
			set_thread_weight(td);
			thread_unlock(td);
			continue;
		}
		rq = thread_rq(td);
		RQ_LOCK(rq);
		on_rq = TD_ON_RUNQ(td);
		running = TD_IS_RUNNING(td);
		if (on_rq || running)
			dequeue_thread(rq, td, 0);
		if (running)
			put_prev_thread(rq, td, 0);
		set_thread_weight(td);
		if (on_rq || running)
			enqueue_thread(rq, td, 0);
		if (running) {
			activate_thread(rq, td);
			TD_SET_RUNNING(td);
		}
		if (delta < 0 || (delta > 0 && running))
			resched_thread(rq_curthread(rq));
		RQ_UNLOCK(rq);
		thread_unlock(td);
	}
}

/*
 * Record the sleep time for the interactivity scorer.
 */
void
sched_sleep(struct thread *td, int prio)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);

	td->td_slptick = ticks;
	if (TD_IS_SUSPENDED(td) || prio >= PSOCK)
		td->td_flags |= TDF_CANSWAP;
	ts = td->td_sched;
	if (PRI_BASE(td->td_pri_class) != PRI_TIMESHARE)
		return;
	if (prio) {
		if (prio_boost == 1)
			sched_prio(td, prio);
		else if (prio < prio_boost)
			sched_prio(td, prio);
	}
}

/*
 * Schedule a thread to resume execution and record how long it voluntarily
 * slept.  We also update the pctcpu, interactivity, and priority.
 */
void
sched_wakeup(struct thread *td)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	td->td_flags &= ~TDF_CANSWAP;
	sched_add(td, 0);
}

/*
 * Penalize the parent for creating a new child and initialize the child's
 * priority.
 */
void
sched_fork(struct thread *td, struct thread *child)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	KASSERT(td == curthread, ("sched_fork() not from curthread?"));
	sched_pctcpu_update(td->td_sched, 1);
	sched_fork_thread(td, child);
}

/*
 * Fork a new thread, may be within the same process.
 */
void
sched_fork_thread(struct thread *td, struct thread *child)
{
	struct td_sched *ts;
	struct td_sched *ts2;
	struct rq *rq;

	/* td may not be the current thread, see kern_kthread.c  XXX */
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	rq = thread_rq(td);
	RQ_LOCK(rq);
	/*
	 * Initialize child.
	 */
	ts = td->td_sched;
	ts2 = child->td_sched;
	child->td_lock = RQ_LOCKPTR(rq);
	child->td_cpuset = cpuset_ref(td->td_cpuset);
	ts2->flags = 0;
	ts2->class = ts->class;
	ts2->se.flags = 0;
	ts2->cpu = ts->cpu;
	ts2->se.joined = 0;
	ts2->se.initial = 1;
	ts2->se.load = ts->se.load;

#ifdef SFQ_GROUP_SCHED
	ts2->group = ts->group;
	sched_group_register_thread(child);
#endif
	set_thread_rq(child, ts2->cpu);

	/*
	 * Do not inherit any borrowed priority from the parent.
	 */
	child->td_priority = child->td_base_pri;
	ts2->ticks = 0;
	ts2->ltick = ts2->ftick = ticks;
	ts2->class->fork_thread(rq, td, child);
	RQ_UNLOCK(rq);
}

static void
check_class_changed(struct rq *rq, struct thread *td,
		const struct sched_class *prev_class)
{
	struct td_sched *ts = td->td_sched;

	if (prev_class != ts->class) {
		if (prev_class->switched_from)
			prev_class->switched_from(rq, td);
		ts->class->switched_to(rq, td);
	}
}

/*
 * Adjust the priority class of a thread.
 */
void
sched_class(struct thread *td, int pri_class)
{
	struct rq *rq;
	struct td_sched *ts;
	struct sched_class *new_class, *prev_class;
	int on_rq, running;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	if (td->td_pri_class == pri_class)
		return;
	ts = td->td_sched;
	rq = thread_rq(td);
	RQ_LOCK(rq);
	prev_class = ts->class;
	new_class = get_sched_class(td, pri_class);
	running = TD_IS_RUNNING(td);
	on_rq = TD_ON_RUNQ(td);
	if (on_rq || running)
		prev_class->dequeue_thread(rq, td, 0);
	if (running) /* clear its curr */
		prev_class->put_prev_thread(rq, td, 0);
	td->td_pri_class = pri_class;
	ts->class = new_class;
	if (on_rq || running)
		ts->class->enqueue_thread(rq, td, 0);
	if (running) {
		ts->class->activate_thread(rq, td);
		TD_SET_RUNNING(td);
	}
	check_class_changed(rq, td, prev_class);
	RQ_UNLOCK(rq);
}

/*
 * Return some of the child's priority and interactivity to the parent.
 */
void
sched_exit(struct proc *p, struct thread *child)
{
	struct thread *td;

	PROC_LOCK_ASSERT(p, MA_OWNED);
	td = FIRST_THREAD_IN_PROC(p);
	sched_exit_thread(td, child);
}

/*
 * Penalize another thread for the time spent on this one.  This helps to
 * worsen the priority and interactivity of processes which schedule batch
 * jobs such as make.  This has little effect on the make process itself but
 * causes new processes spawned by it to receive worse scores immediately.
 */
void
sched_exit_thread(struct thread *td __unused, struct thread *child)
{
#ifdef SFQ_GROUP_SCHED
	sched_group_unregister_thread(child);
#endif
}

/*
 * Fix priorities on return to user-space.  Priorities may be elevated due
 * to static priorities in msleep() or similar.
 */
void
sched_userret(struct thread *td)
{
	/*
	 * XXX we cheat slightly on the locking here to avoid locking in  
	 * the usual case.  Setting td_priority here is essentially an
	 * incomplete workaround for not setting it properly elsewhere.
	 * Now that some interrupt handlers are threads, not setting it
	 * properly elsewhere can clobber it in the window between setting
	 * it here and returning to user mode, so don't waste time setting
	 * it perfectly here.
	 */
	KASSERT((td->td_flags & TDF_BORROWING) == 0,
	    ("thread with borrowed priority returning to userland"));
	if (td->td_priority != td->td_user_pri) {
		thread_lock(td);
		td->td_priority = td->td_user_pri;
		td->td_base_pri = td->td_user_pri;
		//tdq_setlowpri(TDQ_SELF(), td);
		thread_unlock(td);
        }
}

/*
 * Handle a stathz tick.  This is really only relevant for timeshare
 * threads.
 */
void
sched_clock(struct thread *td)
{
	struct pcpuidlestat *stat;
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	ts = td->td_sched;
	sched_pctcpu_update(ts, 1);
	stat = DPCPU_PTR(idlestat);
	stat->oldidlecalls = stat->idlecalls;
	stat->idlecalls = 0;
}

/*
 * Called once per hz tick.
 */
void
sched_tick(int cnt)
{
	struct thread *td = curthread;
	struct td_sched *ts;
	struct rq *rq;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	rq = RQ_SELF();
	MPASS(td->td_lock == RQ_LOCKPTR(rq));
	sched_time_tick();
	update_cpu_load(rq, cnt);
	update_rq_clock(rq);
	ts = td->td_sched;
	ts->class->sched_tick(rq, td);
}

/*
 * Return whether the current CPU has runnable tasks.  Used for in-kernel
 * cooperative idle threads.
 */
int
sched_runnable(void)
{
	struct rq *rq;
	int load;

	load = 1;

	rq = RQ_SELF();
	if ((curthread->td_flags & TDF_IDLETD) != 0) {
		if (rq->nr_running > 0)
			goto out;
	} else
		if (rq->nr_running - 1 > 0)
			goto out;
	load = 0;
out:
	return (load);
}

/*
 * Choose the highest priority thread to run.  The thread is removed from
 * the run-queue while running however the load remains.
 */
struct thread *
sched_choose(void)
{
	struct rq *rq;
	struct thread *td;
	struct sched_class *cls;

	rq = RQ_SELF();

	FOREACH_CLASS(cls) {
		td = cls->pick_next_thread(rq);
		if (td != NULL) {
			td->td_sched->class->activate_thread(rq, td);
			return (td);
		}
	}

	panic("sched_choose");
}

/*
 * Update the percent cpu tracking information when it is requested or
 * the total history exceeds the maximum.  We keep a sliding history of
 * tick counts that slowly decays.  This is less precise than the 4BSD
 * mechanism since it happens with less regular and frequent events.
 */
static void
sched_pctcpu_update(struct td_sched *ts, int run)
{
	int t = ticks;

	if (t - ts->ltick >= SCHED_TICK_TARG) {
		ts->ticks = 0;
		ts->ftick = t - SCHED_TICK_TARG;
	} else if (t - ts->ftick >= SCHED_TICK_MAX) {
		ts->ticks = (ts->ticks / (ts->ltick - ts->ftick)) *
		    (ts->ltick - (t - SCHED_TICK_TARG));
		ts->ftick = t - SCHED_TICK_TARG;
	}
	if (run)
		ts->ticks += (t - ts->ltick) << SCHED_TICK_SHIFT;
	ts->ltick = t;
}

void
check_preempt_curr(struct rq *rq, struct thread *td)
{
	struct thread *ctd;

	check_priority_preempt(rq, td);
	ctd = curthread;
	if (ctd->td_sched->class == td->td_sched->class)
		ctd->td_sched->class->check_preempt_curr(rq, td);
}

/*
 * Select the target thread queue and add a thread to it.  Request
 * preemption or IPI a remote processor if required.
 */
void
sched_add(struct thread *td, int flags)
{
	struct td_sched *ts;
	struct rq *rq;
	int initial;

	THREAD_LOCK_ASSERT(td, MA_OWNED);

	ts = td->td_sched;
	rq = RQ_SELF();
	RQ_LOCK(rq);
	/*
	 * Now that the thread is moving to the run-queue, set the lock
	 * to the scheduler's lock.
	 */
	thread_lock_set(td, RQ_LOCKPTR(rq));
	initial = ts->se.initial;
	ts->se.initial = 0;
	enqueue_thread(rq, td, initial ? 0 : ENQUEUE_WAKEUP);
	check_preempt_curr(rq, td);
}

/*
 * Remove a thread from a run-queue without running it.  This is used
 * when we're stealing a thread from a remote queue.  Otherwise all threads
 * exit by calling sched_exit_thread() and sched_throw() themselves.
 */
void
sched_rem(struct thread *td)
{
	struct rq *rq;

	KASSERT(td != curthread, ("curthread can not be used with sched_rem"));
	rq = thread_rq(td);
	RQ_LOCK_ASSERT(rq, MA_OWNED);
	MPASS(td->td_lock == RQ_LOCKPTR(rq));
	KASSERT(TD_ON_RUNQ(td),
	    ("sched_rem: thread not on run queue"));
	td->td_sched->class->dequeue_thread(rq, td, 0);
	TD_SET_CAN_RUN(td);
}

/*
 * Fetch cpu utilization information.  Updates on demand.
 */
fixpt_t
sched_pctcpu(struct thread *td)
{
	fixpt_t pctcpu;
	struct td_sched *ts;

	pctcpu = 0;
	ts = td->td_sched;
	if (ts == NULL)
		return (0);

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	sched_pctcpu_update(ts, TD_IS_RUNNING(td));
	if (ts->ticks) {
		int rtick;

		/* How many rtick per second ? */
		rtick = min(SCHED_TICK_HZ(ts) / SCHED_TICK_SECS, hz);
		pctcpu = (FSCALE * ((FSCALE * rtick)/hz)) >> FSHIFT;
	}

	return (pctcpu);
}

/*
 * Enforce affinity settings for a thread.  Called after adjustments to
 * cpumask.
 */
void
sched_affinity(struct thread *td)
{
}

/*
 * Bind a thread to a target cpu.
 */
void
sched_bind(struct thread *td, int cpu)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED|MA_NOTRECURSED);
	KASSERT(td == curthread, ("sched_bind: can only bind curthread"));
	ts = td->td_sched;
	if (ts->flags & TSF_BOUND)
		sched_unbind(td);
	KASSERT(THREAD_CAN_MIGRATE(td), ("%p must be migratable", td));
	ts->flags |= TSF_BOUND;
	sched_pin();
	if (PCPU_GET(cpuid) == cpu)
		return;
	ts->cpu = cpu;
	/* When we return from mi_switch we'll be on the correct cpu. */
	mi_switch(SW_VOL, NULL);
}

/*
 * Release a bound thread.
 */
void
sched_unbind(struct thread *td)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	KASSERT(td == curthread, ("sched_unbind: can only bind curthread"));
	ts = td->td_sched;
	if ((ts->flags & TSF_BOUND) == 0)
		return;
	ts->flags &= ~TSF_BOUND;
	sched_unpin();
}
 
int
sched_is_bound(struct thread *td)
{
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	return (td->td_sched->flags & TSF_BOUND);
}

/*
 * Basic yield call.
 */
void
sched_relinquish(struct thread *td)
{
	thread_lock(td);
	mi_switch(SW_VOL | SWT_RELINQUISH, NULL);
	thread_unlock(td);
}

/*
 * Return the total system load.
 */
int
sched_load(void)
{
#ifdef SMP
	int total;
	int i;
                
	total = 0;
	CPU_FOREACH(i)
		total += cpu_rq(i)->nr_sysrunning;
	return (total);
#else
	return (RQ_SELF()->nr_sysrunning);
#endif
}

int
sched_sizeof_proc(void)
{
	return (sizeof(struct proc));
}

int
sched_sizeof_thread(void)
{
	return (sizeof(struct thread) + sizeof(struct td_sched));
}

#ifdef SMP
#define RQ_IDLESPIN(rq)                                               \
    ((rq)->cg != NULL && ((rq)->cg->cg_flags & CG_FLAG_THREAD) == 0)
#else
#define RQ_IDLESPIN(rq)       1
#endif

/*
 * The actual idle process.
 */
void
sched_idletd(void *dummy)
{
	struct pcpuidlestat *stat;

	THREAD_NO_SLEEPING();
	stat = DPCPU_PTR(idlestat);
	for (;;) {
		mtx_assert(&Giant, MA_NOTOWNED);

		while (sched_runnable() == 0) {
			cpu_idle(stat->idlecalls + stat->oldidlecalls > 64);
			stat->idlecalls++;
		}

		thread_lock(curthread);
		mi_switch(SW_VOL | SWT_IDLE, NULL);
		thread_unlock(curthread);
	}
}

/*
 * A CPU is entering for the first time or a thread is exiting.
 */
void
sched_throw(struct thread *td)
{
	struct thread *newtd;
	struct rq *rq;

	rq = RQ_SELF();
	if (td == NULL) {
		/* Correct spinlock nesting and acquire the correct lock. */
		RQ_LOCK(rq);
		spinlock_exit();
		PCPU_SET(switchtime, cpu_ticks());
		PCPU_SET(switchticks, ticks);
	} else {
		MPASS(td->td_lock == RQ_LOCKPTR(rq));
		dequeue_thread(rq, td, 0);
		put_prev_thread(rq, td, 0);
		lock_profile_release_lock(&RQ_LOCKPTR(rq)->lock_object);
	}
	KASSERT(curthread->td_md.md_spinlock_count == 1, ("invalid count"));
	
	newtd = choosethread();
	RQ_LOCKPTR(rq)->mtx_lock = (uintptr_t)newtd;
	cpu_throw(td, newtd);           /* doesn't return */
}

/*
 * This is called from fork_exit().  Just acquire the correct locks and
 * let fork do the rest of the work.
 */
void
sched_fork_exit(struct thread *td)
{
	struct td_sched *ts;
	struct rq *rq;
	int cpuid;
                         
	/*
	 * Finish setting up thread glue so that it begins execution in a
	 * non-nested critical section with the scheduler lock held.
	 */
	cpuid = PCPU_GET(cpuid);
	rq = cpu_rq(cpuid);
	ts = td->td_sched;
	if (TD_IS_IDLETHREAD(td))
		td->td_lock = RQ_LOCKPTR(rq);
	MPASS(td->td_lock == RQ_LOCKPTR(rq));
	td->td_oncpu = cpuid;
	RQ_LOCK_ASSERT(rq, MA_OWNED | MA_NOTRECURSED);
	lock_profile_obtain_lock_success(
		&TDQ_LOCKPTR(tdq)->lock_object, 0, 0, __FILE__, __LINE__);
}

/*
 * Create on first use to catch odd startup conditons.
 */
char *
sched_tdname(struct thread *td)
{
	return (td->td_name);
}

/*
 * This routine determines time constants after stathz and hz are setup.
 */
/* ARGSUSED */
static void
sched_initticks(void *dummy)
{

	realstathz = stathz ? stathz : hz;
	sched_slice = hz / SCHED_SLICE_DEFAULT_DIVISOR;
	sched_slice_min = sched_slice / SCHED_SLICE_MIN_DIVISOR;
	tick_ns = 1000000000 / hz;
	hogticks = 2 * sched_slice;
	sched_time_init();
}

/*
 * Called from proc0_init() to setup the scheduler fields.
 */
void
schedinit(void)
{

	/*
	 * Set up the scheduler specific parts of proc0.
	 */
	proc0.p_sched = NULL; /* XXX */
	thread0.td_sched = &ts0;
	ts0.ltick = ticks;
	ts0.ftick = ticks;
	ts0.class = &pss_class;
}

struct sched_class *
get_sched_class(struct thread *td, int pri_class)
{
	if (pri_class == PRI_ITHD || PRI_BASE(pri_class) == PRI_REALTIME)
		return (&realtime_class);
	if (pri_class == PRI_TIMESHARE)
		return (&pss_class);
	if (pri_class == PRI_IDLE) {
		if (TD_IS_IDLETHREAD(td))
			return (&idle_class);
		return (&pss_class);
	}
	panic("Unknown class");
}

void
update_rq_clock(struct rq *rq)
{
	int64_t delta;

	delta = sched_time_cpu(rq_cpu(rq)) - rq->clock;
	rq->clock += delta;
	rq->clock_thread += delta;
}

/*
 * Initialize a rq.
 */
static void
rq_setup(struct rq *rq)
{
	struct sched_class *cls;

	if (bootverbose)
		printf("SFQ: setup cpu %d\n", rq_cpu(rq));
	mtx_init(&rq->lock, "rq lock", "sched lock",
		MTX_SPIN | MTX_RECURSE);
	rq->clock = sched_time_cpu(rq_cpu(rq));
#ifdef SFQ_GROUP_SCHED
	TAILQ_INIT(&rq->leaf_sfq_list);
#endif
	FOREACH_CLASS(cls) {
 		if (cls->init_rq != NULL)
			cls->init_rq(rq);
	}
#ifdef SMP
	rq->cg = smp_topo_find(cpu_top, rq_cpu(rq));
#endif
}

/*
 * Setup the thread queues and initialize the topology based on MD
 * information.
 */
static void
sched_setup(void *dummy)
{
	struct rq *rq;
#ifdef SFQ_GROUP_SCHED
	unsigned long alloc_size;
	uintptr_t ptr;
#endif

#ifdef SMP
	int cpu;
#endif
	cpupri_init(&root_cpupri);

	pss_sched_init();

#ifdef SMP
	CPU_FOREACH(cpu) {
		rq = cpu_rq(cpu);
		rq_setup(rq);
	}
#else
	rq_setup(RQ_SELF());
#endif

	rq = RQ_SELF();

#ifdef SFQ_GROUP_SCHED
	mtx_init(&sg_id_lock, "sched_group id lock", NULL, MTX_DEF);
	mtx_init(&sched_group_lock, "sched group lock", NULL, MTX_SPIN);
	sx_init(&sg_tree_lock, "sched_group tree lock");
	TAILQ_INIT(&all_sched_groups);
	sg_id_unrhdr = new_unrhdr(1, INT_MAX, &sg_id_lock);
	entity_zone = uma_zcreate("sched_entity", sizeof(struct sched_entity),
		NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	sfq_zone = uma_zcreate("sched_entity", sizeof(struct sfq),
		NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	group_zone = uma_zcreate("sched_group", sizeof(struct sched_group),
		NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	alloc_size = sizeof(void **) * mp_ncpus * 2;
	ptr = (uintptr_t)malloc(alloc_size, M_SCHED, M_NOWAIT);
	root_sched_group.se = (struct sched_entity **)ptr;
	ptr += sizeof(void **) * mp_ncpus;
	root_sched_group.sfq = (struct sfq **)ptr;
	TAILQ_INIT(&root_sched_group.children);
	TAILQ_INIT(&root_sched_group.threads);
	TAILQ_INSERT_TAIL(&root_sched_group.threads, &ts0, glink);
	TAILQ_INSERT_TAIL(&all_sched_groups, &root_sched_group, link);
	pss_init_group(&root_sched_group, &rq->sfq, NULL, 0, NULL);
#endif

	/* Add thread0's load since it's running. */
	RQ_LOCK(rq);
	thread0.td_lock = RQ_LOCKPTR(rq);
	thread0.td_base_user_pri = NORMAL_PRI;
	thread0.td_user_pri = NORMAL_PRI;
	set_thread_weight(&thread0);
#ifdef SFQ_GROUP_SCHED
	thread0.td_sched->group = &root_sched_group;
#endif
	set_thread_rq(&thread0, 0);
	enqueue_thread(rq, &thread0, 0);
	activate_thread(rq, &thread0);
	RQ_UNLOCK(rq);
}

#ifdef SFQ_GROUP_SCHED

static void
sched_group_register_thread(struct thread *td)
{
	struct td_sched *ts = td->td_sched;

	mtx_lock_spin(&sched_group_lock);
	TAILQ_INSERT_TAIL(&ts->group->threads, ts, glink);
	mtx_unlock_spin(&sched_group_lock);
}

static void
sched_group_unregister_thread(struct thread *td)
{
	struct td_sched *ts = td->td_sched;

	mtx_lock_spin(&sched_group_lock);
	TAILQ_REMOVE(&ts->group->threads, ts, glink);
	mtx_unlock_spin(&sched_group_lock);
}

static void
free_sched_group(struct sched_group *sg)
{
	pss_free_sched_group(sg);
	uma_zfree(group_zone, sg);
}

static struct sched_group *
sched_group_find(int id)
{
	struct sched_group *sg;

	TAILQ_FOREACH(sg, &all_sched_groups, link) {
		if (sg->id == id)
			return (sg);
	}
	return (NULL);
}

static struct sched_group *
sched_group_find_by_name(struct sched_group *parent, const char *name)
{
	struct sched_group *sg;

	TAILQ_FOREACH(sg, &parent->children, sibling) {
		if (strcmp(sg->name, name) == 0)
			return (sg);
	}
	return (sg);
}

static void
sched_group_move(struct thread *td, struct sched_group *sg)
{
	struct rq *rq;
	int on_rq, running;

	thread_lock(td);
	rq = thread_rq(td);
	RQ_LOCK(rq);
	on_rq = TD_ON_RUNQ(td);
	running = TD_IS_RUNNING(td);
	if (on_rq || running)
		dequeue_thread(rq, td, 0);
	if (running)
		put_prev_thread(rq, td, 0);
	td->td_sched->group = sg;
	if (td->td_sched->class->sched_group_move)
		td->td_sched->class->sched_group_move(td, on_rq || running);
	else
		set_thread_rq(td, thread_cpu(td));
	if (on_rq || running)
		enqueue_thread(rq, td, 0);
	if (running) {
		activate_thread(rq, td);
		TD_SET_RUNNING(td);
	}
	RQ_UNLOCK(rq);
	thread_unlock(td);
}

static struct sched_group *
sched_group_create(struct sched_group *parent,
	struct sched_group_create_param *param)
{
	struct sched_group *sg;
	unsigned shares;

	MPASS(parent != NULL);

	sg = uma_zalloc(group_zone, M_WAITOK | M_ZERO);
	if (!sg)
		return (NULL);
	if (param) {
		strcpy(sg->name, param->name);
		shares = param->shares;
	} else
		shares = 0;
	sg->parent = parent;
	TAILQ_INIT(&sg->children);
	TAILQ_INIT(&sg->threads);
	if (!pss_alloc_sched_group(sg, parent, shares))
		goto err;

	sg->id = alloc_unr(sg_id_unrhdr);
	sg->parent = parent;
	TAILQ_INSERT_TAIL(&parent->children, sg, sibling);
	TAILQ_INSERT_TAIL(&all_sched_groups, sg, link);

	return (sg);
err:
	free_sched_group(sg);
	return (NULL);
}

static int
sched_group_destroy(struct sched_group *sg)
{
	int i;

	if (!TAILQ_EMPTY(&sg->children))
		return (EBUSY);
	mtx_lock_spin(&sched_group_lock);
	if (!TAILQ_EMPTY(&sg->threads)) {
		mtx_unlock_spin(&sched_group_lock);
		return (EBUSY);
	}
	mtx_unlock_spin(&sched_group_lock);
	CPU_FOREACH(i) {
		pss_unregister_sched_group(sg, i);
	}
	pss_free_sched_group(sg);
	TAILQ_REMOVE(&sg->parent->children, sg, sibling);
	TAILQ_REMOVE(&all_sched_groups, sg, link);
	free_unr(sg_id_unrhdr, sg->id);
	uma_zfree(group_zone, sg);
	return (0);
}

static int
sysctl_sched_group_create(SYSCTL_HANDLER_ARGS)
{
	struct sched_group *parent, *child;
	int *name = (int *)arg1;
	u_int namelen = arg2;
	int error = 0;
	struct sched_group_create_param param;

	if (namelen < 1)
		return (EINVAL);
	if (req->oldptr == NULL || req->oldlen < sizeof(int))
		return (EINVAL);
	if (req->newptr) {
		if (req->newlen < sizeof(param))
			return (EINVAL);
		error = SYSCTL_IN(req, &param, sizeof(param));
		if (error)
			return (error);
		param.name[SCHED_GROUP_NAME_LEN-1] = 0;
	}
	sx_xlock(&sg_tree_lock);
	parent = sched_group_find(*name);
	if (parent == NULL) {
		sx_unlock(&sg_tree_lock);
		return (ESRCH);
	}
	if (req->newptr && param.name[0]) {
		child = sched_group_find_by_name(parent, param.name);
		if (child) {
			sx_unlock(&sg_tree_lock);
			error = EEXIST;
			return (error);
		}
	}
	child = sched_group_create(parent, req->newptr ? &param : NULL);
	sx_unlock(&sg_tree_lock);
	if (child != NULL) {
		int id;
		id = child->id;
		error = SYSCTL_OUT(req, &id, sizeof(int));
	}
	return (error);
}

static int
sysctl_sched_group_move(SYSCTL_HANDLER_ARGS)
{
	struct sched_group *sg, *old;
	struct proc *p;
	struct thread *td;
	int *name = (int *)arg1;
	u_int namelen = arg2;
	int id;
	int error;

	if (namelen < 1)
		return (EINVAL);
	if (req->newptr == NULL)
		return (EINVAL);
	error = SYSCTL_IN(req, &id, sizeof(int));
	if (error)
		return (error);
	sx_xlock(&sg_tree_lock);
	p = pfind(*name);
	if (p == NULL) {
		sx_unlock(&sg_tree_lock);
		return (ESRCH);
	}
	sg = sched_group_find(id);
	if (sg == NULL) {
		error = ESRCH;
		goto done;
	}
	FOREACH_THREAD_IN_PROC(p, td) {
		old = td->td_sched->group;
		if (old == sg)
			continue;
		mtx_lock_spin(&sched_group_lock);
		TAILQ_REMOVE(&old->threads, td->td_sched, glink);
		TAILQ_INSERT_TAIL(&sg->threads, td->td_sched, glink);
		mtx_unlock_spin(&sched_group_lock);
		sched_group_move(td, sg);
	}
done:
	PROC_UNLOCK(p);
	sx_unlock(&sg_tree_lock);
	return (error);
}

static int
sysctl_sched_group_set_shares(SYSCTL_HANDLER_ARGS)
{
	struct sched_group *sg;
	int *name = (int *)arg1;
	u_int namelen = arg2;
	unsigned int shares, old_shares;
	int error = 0;

	if (namelen < 1)
		return (EINVAL);
	if (req->newptr != NULL) {
		error = SYSCTL_IN(req, &shares, sizeof(int));
		if (error)
			return (error);
	}
	sx_slock(&sg_tree_lock);
	sg = sched_group_find(*name);
	if (sg == NULL) {
		error = ESRCH;
		goto done;
	}
	if (req->oldptr != NULL) {
		if (req->oldlen < sizeof(int)) {
			error = EINVAL;
			goto done;
		}
		old_shares = sg->shares;
		error = SYSCTL_OUT(req, &old_shares, sizeof(int));
		if (error)
			goto done;	
	}
	if (req->newptr != NULL)
		pss_sched_group_set_shares(sg, shares);
done:
	sx_sunlock(&sg_tree_lock);
	return (error);
}

static int
sysctl_sched_group_get_sub_groups(SYSCTL_HANDLER_ARGS)
{
	struct sched_group *sg, *tmp;
	struct sched_group_info info;
	int *name = (int *)arg1;
	u_int namelen = arg2;
	int error = 0;

	if (namelen < 1)
		return (EINVAL);
	sx_slock(&sg_tree_lock);
	sg = sched_group_find(*name);
	if (sg == NULL) {
		error = ESRCH;
		goto done;
	}
	TAILQ_FOREACH(tmp, &sg->children, sibling) {
		info.parent_id = *name;
		info.id = tmp->id;
		strcpy(info.name, tmp->name);
		info.shares = tmp->shares;
		error = SYSCTL_OUT(req, &info, sizeof(info));
	}
done:
	sx_sunlock(&sg_tree_lock);
	return (error);
}

static int
sysctl_sched_group_destroy(SYSCTL_HANDLER_ARGS)
{
	struct sched_group *sg;
	int *name = (int *)arg1;
	u_int namelen = arg2;
	int error = 0;

	if (namelen < 1)
		return (EINVAL);
	sx_slock(&sg_tree_lock);
	sg = sched_group_find(*name);
	if (sg == NULL)
		error = ESRCH;
	else
		error = sched_group_destroy(sg);
	sx_sunlock(&sg_tree_lock);
	return (error);
}

SYSCTL_NODE(_kern_sched, OID_AUTO, group, CTLFLAG_RD,  0, "sched group");
static SYSCTL_NODE(_kern_sched_group, OID_AUTO, create,
	CTLFLAG_RW | CTLFLAG_MPSAFE,
	sysctl_sched_group_create, "create sched_group");
static SYSCTL_NODE(_kern_sched_group, OID_AUTO, move, 
	CTLFLAG_RW | CTLFLAG_MPSAFE,
	sysctl_sched_group_move, "move into group");
static SYSCTL_NODE(_kern_sched_group, OID_AUTO, shares, 
	CTLFLAG_RW | CTLFLAG_MPSAFE,
	sysctl_sched_group_set_shares, "set group shares");
static SYSCTL_NODE(_kern_sched_group, OID_AUTO, subgroup, 
	CTLFLAG_RD | CTLFLAG_MPSAFE,
	sysctl_sched_group_get_sub_groups, "get sub-groups");
static SYSCTL_NODE(_kern_sched_group, OID_AUTO, destroy,
	CTLFLAG_RD | CTLFLAG_MPSAFE,
	sysctl_sched_group_destroy, "destroy groups");
#endif

SYSCTL_NODE(_kern, OID_AUTO, sched, CTLFLAG_RW, 0, "Scheduler");
SYSCTL_STRING(_kern_sched, OID_AUTO, name, CTLFLAG_RD, "SFQ", 0,
    "Scheduler name");
SYSCTL_INT(_kern_sched, OID_AUTO, prio_boost, CTLFLAG_RW, &prio_boost, 0,
    "allow priority boost for sleeping threads");
/* ps compat.  All cpu percentages from SFQ are weighted. */
static int ccpu = 0;
SYSCTL_INT(_kern, OID_AUTO, ccpu, CTLFLAG_RD, &ccpu, 0, "");
SYSCTL_LONG(_kern_sched, OID_AUTO, load0, CTLFLAG_RD, &rqs[0].cpu_load[0],
	0, "");
SYSCTL_LONG(_kern_sched, OID_AUTO, load1, CTLFLAG_RD, &rqs[0].cpu_load[1],
	0, "");
SYSCTL_LONG(_kern_sched, OID_AUTO, load2, CTLFLAG_RD, &rqs[0].cpu_load[2],
	0, "");
SYSCTL_LONG(_kern_sched, OID_AUTO, load3, CTLFLAG_RD, &rqs[0].cpu_load[3],
	0, "");
SYSCTL_LONG(_kern_sched, OID_AUTO, load4, CTLFLAG_RD, &rqs[0].cpu_load[4],
	0, "");
