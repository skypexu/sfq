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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/sched.h>
#include <sys/schedgroup.h>
#include <sys/tree.h>
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/uma.h>

#include <sys/schedgroup.h>

#if defined(RT_GROUP_SCHED) || defined(SFQ_GROUP_SCHED)
#ifndef GROUP_SCHED
#define GROUP_SCHED
#endif
#endif

#include "hrtimer.h"

#define RUNTIME_INF		((uint64_t)~0ULL)
#define	MAX_RT_PRI		(PUSER)
#define	NORMAL_PRI		(PUSER+1)
#define INVALID_PRI		(-1)
#define IDLE_PRI		(PRI_MAX+1)
/* Number of rt_prio + 1 pss_pri + 1 idle_pri */
#define CPUPRI_NR		((MAX_RT_PRI + 1) + 1 + 1)

#ifdef SMP
#define	SMP_MB()		mb()
#else
#define	SMP_MB()
#endif

struct cpupri_vec {
	int		count;
	cpuset_t	mask;
};

struct cpupri {
	struct cpupri_vec	pri_to_cpu[CPUPRI_NR];
	int			cpu_to_pri[MAXCPU];
};

struct rt_bandwidth {
	struct mtx		rt_runtime_lock;
	struct timespec		rt_period;
	uint64_t		rt_runtime;
	struct hrtimer		rt_period_timer;
};

#define	DEQUEUE_SLEEP	0x0001
#define ENQUEUE_HEAD	0x0001
#define ENQUEUE_WAKEUP	0x0002

struct rq;

/* Schedule entity class */
struct sched_class {
	struct sched_class *next;
	char 	*name;

	void	(*init_rq)(struct rq *);
	void	(*put_prev_thread)(struct rq *, struct thread *, int);
	struct thread *	(*pick_next_thread)(struct rq *);
	void	(*activate_thread)(struct rq *, struct thread *);
	void	(*sched_tick)(struct rq *, struct thread *);
	void	(*change_priority)(struct rq *, struct thread *, u_char);
	void	(*check_preempt_curr)(struct rq *, struct thread *);
	void	(*enqueue_thread)(struct rq *, struct thread *, int);
	void	(*dequeue_thread)(struct rq *, struct thread *, int);
	void	(*fork_thread)(struct rq *, struct thread *, struct thread *);
	void	(*switched_from)(struct rq *, struct thread *);
	void	(*switched_to)(struct rq *, struct thread *);
#ifdef GROUP_SCHED
	void	(*sched_group_move)(struct thread *, int);
#endif
};

#ifdef GROUP_SCHED

struct td_sched;

struct sched_group {
	int	id;
	char	name[SCHED_GROUP_NAME_LEN];

#ifdef SFQ_GROUP_SCHED
	/* Schedulable entities of this group on each cpu */
	struct sched_entity	**se;
	/* Runqueue owned by this group on each cpu */
	struct sfq	**sfq;
	unsigned long	shares;
	unsigned long	load_weight;
#endif

#ifdef RT_GROUP_SCHED
	struct sched_rt_entity **rt_se;
	struct rt_rq **rt_rq;
	struct rt_bandwidth rt_bandwidth;
#endif

	struct sched_group		*parent;
	TAILQ_ENTRY(sched_group)	link;  /* List of all groups */
	TAILQ_ENTRY(sched_group)	sibling; /* Sibling group */
	TAILQ_HEAD(,sched_group)	children;/* Child groups */
	TAILQ_HEAD(,td_sched)		threads; /* All threads */
};
#endif

struct load_weight {
	unsigned long	weight;
	unsigned long	inv_weight;
};

/*
 * Thread scheduler specific section.  All fields are protected
 * by the thread lock.
 */
struct sched_entity {
	int		flags;		/* SEF_* flags. */
	uint64_t	vruntime;	/* Effective virtual time */
	uint64_t	timing_start;
	uint64_t	sum_exec_time;
	uint64_t	prev_sum_exec_time;
	int		initial;
	int		joined;
	struct load_weight	load;
	RB_ENTRY(sched_entity)	tlink;

#ifdef SFQ_GROUP_SCHED
	struct sched_entity	*parent;
	struct sfq	*sfq;		/* queue this entity is queued */
	struct sfq	*my_q;		/* queue owned by this entity/group */
#endif
};

#define	SEF_ONSFQ	0x0001
#define	SEF_ONRUNQ	0x0002

struct sched_rt_entity {
	TAILQ_ENTRY(sched_rt_entity) link;
	int		joined;
	uint64_t	timing_start;

#ifdef RT_GROUP_SCHED
	struct sched_rt_entity  *parent;
	/* rq on which this entity is (to be) queued: */
	struct rt_rq            *rt_rq;
	/* rq "owned" by this entity/group: */
	struct rt_rq            *my_q;
#endif
};

TAILQ_HEAD(tdprioq, td_sched);
struct td_sched {
	struct sched_entity	se;
	struct sched_rt_entity	rt_se;
	struct sched_class	*class;
	struct sched_group	*group;
	TAILQ_ENTRY(td_sched)	glink;
	int			flags;
#define	TSF_BOUND	0x0001	/* Thread can not migrate. */
#define	TSF_XFERABLE	0x0002	/* Thread was added as transferable. */
	int			cpu;
	int			rltick;	/* Real last tick, for affinity. */
	int			ltick;	/* Last tick that we were running on */
	int			ftick;	/* First tick that we were running on */
	int			ticks;	/* Tick count */

	int			prio; /* saved priority in prio list. */
	struct tdprioq		qhead; /* thread queue in same prio. */
	TAILQ_ENTRY(td_sched)	qlink; /* link for same priority. */
	TAILQ_ENTRY(td_sched)	llink; /* pushable list link */
};

typedef	RB_HEAD(TIMELINE, sched_entity)     TIMELINE;

struct sfq {
	TIMELINE		timeline;
	struct sched_entity	*leftmost, *curr, *skip, *next, *last;
	uint64_t		vtime;
	uint64_t		vtime_copy;
	int			nr_running;
	struct load_weight	load;

#ifdef SFQ_GROUP_SCHED
	struct rq		*rq;
	struct sched_group	*group;
	TAILQ_ENTRY(sfq)	leaf_link;
	int			on_leaf_list;
	/* Last time we have visited the load. */
	uint64_t		load_timestamp;
	/* Last time we have found load. */
	uint64_t		load_laststamp;
	uint64_t		load_period;
	uint64_t		load_avg;
	uint64_t		load_pending_exec_time;
	unsigned long		load_contribution;
#endif
};

#define NUM_RT_PRIO	(PRI_MAX+1)

BITSET_DEFINE(rt_bitmap, NUM_RT_PRIO+1)
TAILQ_HEAD(rt_head, sched_rt_entity);
TAILQ_HEAD(rt_rq_head, rt_rq);

/*
 * This is the priority-queue data structure of the RT scheduling class:
 */
struct rt_prio_array {
	struct rt_bitmap bitmap;
	struct rt_head   queue[NUM_RT_PRIO];
};

struct prio_list {
	TAILQ_HEAD(,td_sched)	head;
};

/* Real-Time classes' related field in a runqueue: */
struct rt_rq {
	struct rt_prio_array active;
	unsigned int rt_nr_running;
#if defined(SMP) || defined(RT_GROUP_SCHED)
	struct {
		int curr; /* highest queued rt task prio */
#ifdef SMP
		int next; /* next highest */
#endif  
	} highest_prio;
#endif
#ifdef SMP
	unsigned long rt_nr_migratory;
	unsigned long rt_nr_total;
	int overloaded;
	struct prio_list pushable_threads;
#endif
	int rt_throttled;
	uint64_t rt_time;
	uint64_t rt_runtime;
	struct mtx rt_runtime_lock;
  
#ifdef RT_GROUP_SCHED
	unsigned long rt_nr_boosted;
	struct rq *rq;
	struct rt_rq_head leaf_rt_rq_list;
	struct sched_group *sg;
#endif
};

#define CPU_LOAD_IDX_MAX	5

/*
 * rq - per processor runqs and statistics.  All fields are protected by 
 * per-queue lock.
 */
struct rq {
	struct mtx	lock;		/* run queue lock. */
	struct cpu_group	*cg;	/* Pointer to cpu topology. */
	volatile int	nr_running;	/* Aggregate load. */
	volatile int	cpu_idle;	/* cpu_idle() is active. */
	int		nr_sysrunning;	/* For loadavg, !ITHD load. */
	struct runq	realtime;	/* real-time run queue. */
	struct sfq	sfq;		/* timeshare run queue. */
	struct rt_rq	rt;
	struct sched_entity 	*rt_curr;
	struct load_weight	load;
	uint64_t	clock;
	uint64_t	clock_thread;

	int		transferable;	/* Transferable thread count. */
	short		switchcnt;	/* Switches this tick. */
	short		oldswitchcnt;	/* Switches last tick. */
	u_char		ipipending;	/* IPI pending. */

#ifdef SFQ_GROUP_SCHED
	TAILQ_HEAD(,sfq) leaf_sfq_list;
#endif
	unsigned long	cpu_load[CPU_LOAD_IDX_MAX];
	unsigned long	last_cpu_load_tick;
} __aligned(64);

extern struct rq rqs[];
extern struct td_sched ts0;

static inline uint64_t
timespec_to_ns(const struct timespec *ts)
{
	return ((uint64_t)ts->tv_sec * 1000000000ULL + ts->tv_nsec);
}

static inline void
ns_to_timespec(struct timespec *ts, uint64_t ns)
{
	ts->tv_sec  = ns / 1000000000;
	ts->tv_nsec = ns % 1000000000;
}

static inline int
rq_cpu(struct rq *rq)
{
	return (rq - rqs);
}

static inline struct rq *
cpu_rq(int cpu)
{
	return &rqs[cpu];
}

static inline int
thread_cpu(struct thread *td)
{
	return td->td_sched->cpu;
}

static inline struct rq *
thread_rq(struct thread *td)
{
	return cpu_rq(thread_cpu(td));
}

static inline int
entity_is_thread(struct sched_entity *se)
{
#ifdef SFQ_GROUP_SCHED
	return (!se->my_q);
#else
	return (1);
#endif
}

static inline struct sched_entity *
td_to_se(struct thread *td)
{
	return (&td->td_sched->se);
}

static inline struct thread *
se_to_td(struct sched_entity *se)
{
	struct td_sched *ts;

#ifdef SFQ_GROUP_SCHED
	MPASS(entity_is_thread(se));
#endif
	if (__predict_false((char *)se == (char *)&ts0.se))
		return (&thread0);
	ts =  __containerof(se, struct td_sched, se);
	return ((struct thread *)ts - 1);
}

static inline int
rt_entity_is_thread(struct sched_rt_entity *rt_se)
{
#ifdef RT_GROUP_SCHED
	return (!rt_se->my_q);
#else
	return (1);
#endif
}

static inline struct sched_rt_entity *
td_to_rt_se(struct thread *td)
{
	return (&td->td_sched->rt_se);
}

static inline struct thread *
rt_se_to_td(struct sched_rt_entity *rt_se)
{
	struct td_sched *ts;

#ifdef RT_GROUP_SCHED
	MPASS(rt_entity_is_thread(rt_se));
#endif
	if (__predict_false((char *)rt_se == (char *)&ts0.rt_se))
		return (&thread0);
	ts =  __containerof(rt_se, struct td_sched, rt_se);
	return ((struct thread *)ts - 1);
}

static inline struct thread *
ts_to_td(struct td_sched *ts)
{
	if (__predict_false(ts == &ts0))
		return (&thread0);
	return ((struct thread *)ts - 1);
}

#define	RQ_SELF()		cpu_rq(PCPU_GET(cpuid))
#define	RQ_LOCK_ASSERT(t, type)	mtx_assert(RQ_LOCKPTR((t)), (type))
#define	RQ_LOCK(t)		mtx_lock_spin(RQ_LOCKPTR((t)))
#define	RQ_LOCK_FLAGS(t, f)	mtx_lock_spin_flags(RQ_LOCKPTR((t)), (f))
#define	RQ_UNLOCK(t)		mtx_unlock_spin(RQ_LOCKPTR((t)))
#define	RQ_LOCKPTR(t)		((struct mtx *)(&(t)->lock))

/* Flags kept in td_flags. */
#define	TDF_SLICEEND	TDF_SCHED2	/* Thread time slice is over. */

struct sched_class * get_sched_class(struct thread *td, int);
void	rq_run_add(struct rq *, struct thread *);
void	rq_run_rem(struct rq *, struct thread *);
int	rq_slice(struct rq *);
uint64_t	rq_slice_ns(struct rq *);
void	check_preempt_curr(struct rq *, struct thread *);
void	check_priority_preempt(struct rq *, struct thread *);
void	set_thread_weight(struct thread *);

extern struct sched_class realtime_class;
extern struct sched_class pss_class;
extern struct sched_class idle_class;
extern struct td_sched ts0;

#define FOREACH_CLASS(class)		\
	for (class = &realtime_class; class != NULL; class = class->next)

uint64_t sched_time(void);

MALLOC_DECLARE(M_SCHED);

#define MIN_SHARES	(1UL <<  1)
#define MAX_SHARES	(1UL << 18)

#ifdef GROUP_SCHED
extern uma_zone_t	group_zone;
extern struct sched_group	root_sched_group;
#endif

#ifdef SFQ_GROUP_SCHED
extern uma_zone_t	entity_zone;
extern uma_zone_t	sfq_zone;
void	pss_init_group(struct sched_group *, struct sfq *,
		struct sched_entity *, int, struct sched_entity *);
int	pss_alloc_sched_group(struct sched_group *, struct sched_group *, unsigned);
void	pss_free_sched_group(struct sched_group *);
int	pss_sched_group_set_shares(struct sched_group *, unsigned long);
void	pss_unregister_sched_group(struct sched_group *, int);
#endif

void pss_sched_init(void);

static __inline void
set_thread_rq(struct thread *td, unsigned int cpu)
{
#ifdef SFQ_GROUP_SCHED
	struct sched_group *tg = td->td_sched->group;
	td->td_sched->se.sfq = tg->sfq[cpu];
	td->td_sched->se.parent = tg->se[cpu];
#endif
}

static __inline uint64_t
wrap_min_64(uint64_t x, uint64_t y)
{
	return ((int64_t)(x - y) < 0 ? x : y);
}

static __inline uint64_t
wrap_max_64(uint64_t x, uint64_t y)
{
	return ((int64_t)(x - y) > 0 ? x : y);
}

static __inline int
thread_need_resched(struct thread *td)
{
	return (td->td_flags & TDF_NEEDRESCHED);
}

static __inline void
resched_thread(struct thread *td)
{
	td->td_flags |= TDF_NEEDRESCHED;
}

static __inline void
preempt_thread(struct thread *td)
{
#ifdef SMP
	int cpu;
#endif

	if (!(td->td_flags & TDF_NEEDRESCHED))
		td->td_flags |= TDF_NEEDRESCHED;
#ifdef SMP
	cpu = thread_cpu(td);
	if (cpu != PCPU_GET(cpuid))
		ipi_cpu(cpu, IPI_PREEMPT);
	else
		td->td_owepreempt = 1;
#else
	td->td_owepreempt = 1;
#endif
}

static __inline struct thread *
rq_curthread(struct rq *rq)
{
	return pcpu_find(rq_cpu(rq))->pc_curthread;
}

extern int sysctl_sched_rt_runtime;

static inline int rt_bandwidth_enabled(void)
{
	return (sysctl_sched_rt_runtime >= 0);
}

void update_rq_clock(struct rq *);
void sched_time_init(void);
void sched_time_tick(void);
uint64_t sched_time_cpu(int);

int cpupri_find(struct cpupri *, struct thread *, cpuset_t *);
void cpupri_set(struct cpupri *, int, int);
int cpupri_init(struct cpupri *);

void prio_list_init(struct prio_list *);
struct thread * prio_list_first(struct prio_list *);
int prio_list_empty(struct prio_list *);
void prio_list_add(struct prio_list *, struct thread *);
void prio_list_del(struct prio_list *, struct thread *);
void init_rt_bandwidth(struct rt_bandwidth *, uint64_t, uint64_t);
void init_rt_rq(struct rt_rq *, struct rq *);
void start_bandwidth_timer(struct hrtimer *, const struct timespec *);

void rt_free_sched_group(struct sched_group *sg);
void rt_init_group(struct sched_group *, struct rt_rq *,
	struct sched_rt_entity *, int, struct sched_rt_entity *);
int rt_alloc_sched_group(struct sched_group *, struct sched_group *);
